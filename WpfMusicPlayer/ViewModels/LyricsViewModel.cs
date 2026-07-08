// SPDX-License-Identifier: MIT

using System.Collections.ObjectModel;
using System.IO;
using System.Text;
using System.Text.Encodings.Web;
using System.Text.Json;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using Microsoft.Extensions.Logging;
using WpfMusicPlayer.Helpers;
using WpfMusicPlayer.Models.Lyrics;
using WpfMusicPlayer.Services.Abstractions;
using WpfMusicPlayer.Services.Implementations;

namespace WpfMusicPlayer.ViewModels;

public enum SuppliedLyricSource
{
    Embedded,
    DatabaseCache
}

public partial class LyricsViewModel(
    ILogger<LyricsViewModel> logger,
    IFileDialogService fileDialogService,
    LyricParserFactory lyricParserFactory) : ObservableObject
{
    private const string WplrcExtension = ".wplrc";
    private const string InterludeLyricText = "♪ ♪ ♪ ♪ ♪ ♪";
    private const int InterludeLyricGapThresholdMs = 5000;

    private static readonly IReadOnlyDictionary<string, IReadOnlyList<string>> WplrcSaveFileTypeChoices =
        new Dictionary<string, IReadOnlyList<string>>
        {
            ["WPLRC 歌词"] = [WplrcExtension]
        };

    private static readonly JsonSerializerOptions PrettyJsonOptions = new()
    {
        Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
        WriteIndented = true
    };

    private static readonly Encoding Utf8NoBom = new UTF8Encoding(encoderShouldEmitUTF8Identifier: false);

    private readonly List<LyricState> _lyricStates = [];

    public event Action<float>? SeekRequested;
    public event Action<string, int>? UpdateCurrentLyricRequested;
    public event Action<int>? UpdateCurrentLyricOffsetRequested;

    private double _currentLyricDuration;
    private double _lastPlaybackTimeSec;
    private int _currentLyricOffsetMs;
    private string? _currentIntermediateLyricJson;
    private string _suggestedWplrcFileName = "lyrics";
    private static readonly LyricLineViewModel EmptyLyric = new("暂无歌词");

    public ObservableCollection<LyricLineViewModel> Lyrics { get; } = [];

    [ObservableProperty]
    public partial int CurrentLyricIndex { get; private set; } = -1;

    public LyricLineViewModel CurrentLyric
    {
        get
        {
            var idx = CurrentLyricIndex;
            if (idx >= 0 && idx < Lyrics.Count)
                return Lyrics[idx];

            return EmptyLyric;
        }
    }

    [ObservableProperty]
    public partial bool IsTranslationVisible { get; set; } = true;

    [ObservableProperty]
    public partial bool HasTranslationAvailable { get; private set; }

    [ObservableProperty]
    public partial bool IsRomanjiVisible { get; set; } = true;

    [ObservableProperty]
    public partial bool HasRomanjiAvailable { get; private set; }

    [ObservableProperty]
    public partial RomanizationSchema CurrentRomanizationSchema { get; private set; } =
        RomanizationSchema.ErrorOrNotEnabled;

    public string RomanjiButtonContent =>
        CurrentRomanizationSchema == RomanizationSchema.Jyutping ? "拼" : "あ";

    [RelayCommand]
    private void ToggleTranslation()
    {
        IsTranslationVisible = !IsTranslationVisible;
    }

    [RelayCommand]
    private void ToggleRomanji()
    {
        IsRomanjiVisible = !IsRomanjiVisible;
    }

    public void SeekToLyric(LyricLineViewModel lyric)
    {
        if (lyric.TimeMs < 0) return;
        logger.LogInformation("Seek to lyric requested, time = {LyricTimeMs} (in MS)", lyric.TimeMs);
        SeekRequested?.Invoke(lyric.TimeMs / 1000f);
    }

    public void ResetState()
    {
        _lyricStates.Clear();
        _currentLyricDuration = 0;
        _lastPlaybackTimeSec = 0;
        _currentLyricOffsetMs = 0;
        _currentIntermediateLyricJson = null;
        _suggestedWplrcFileName = "lyrics";
        Lyrics.Clear();
        CurrentLyricIndex = -1;
        OnPropertyChanged(nameof(CurrentLyric));
        HasTranslationAvailable = false;
        HasRomanjiAvailable = false;
        CurrentRomanizationSchema = RomanizationSchema.ErrorOrNotEnabled;
        ExportWplrcCommand.NotifyCanExecuteChanged();
    }

    public void UpdateLyricProgress(double time)
    {
        _lastPlaybackTimeSec = time;
        if (_lyricStates.Count == 0) return;

        var currentTimeMs = (int)Math.Round(time * 1000);
        var rawTimeMs = currentTimeMs - _currentLyricOffsetMs;
        var newIndex = GetLyricIndexAtRawTime(rawTimeMs);

        if (newIndex != CurrentLyricIndex && newIndex >= 0 && newIndex < Lyrics.Count)
        {
            if (CurrentLyricIndex >= 0 && CurrentLyricIndex < Lyrics.Count)
            {
                Lyrics[CurrentLyricIndex].IsHighlighted = false;
                Lyrics[CurrentLyricIndex].Progress = 0;
            }
            CurrentLyricIndex = newIndex;
            Lyrics[CurrentLyricIndex].IsHighlighted = true;
        }

        if (CurrentLyricIndex >= 0 && CurrentLyricIndex < Lyrics.Count)
        {
            var current = Lyrics[CurrentLyricIndex];
            current.Progress = current.IsProgressEnabled
                ? CalculateControllerLyricProgress(_lyricStates[CurrentLyricIndex], rawTimeMs)
                : CalculateLinearLyricProgress(CurrentLyricIndex, time);
        }
    }

    public void OnPlaybackStopped()
    {
        _lastPlaybackTimeSec = 0;
        if (_lyricStates.Count == 0) return;
        if (CurrentLyricIndex >= 0 && CurrentLyricIndex < Lyrics.Count)
        {
            Lyrics[CurrentLyricIndex].IsHighlighted = false;
            Lyrics[CurrentLyricIndex].Progress = 0;
        }
        CurrentLyricIndex = 0;
        Lyrics[CurrentLyricIndex].IsHighlighted = true;
        Lyrics[CurrentLyricIndex].Progress = 0;
    }

    public async Task LoadLyricsAsync(
        string? filePath,
        string? suppliedLyric,
        string? songTitle,
        double songDuration,
        int offsetMs,
        SuppliedLyricSource suppliedLyricSource = SuppliedLyricSource.Embedded)
    {
        logger.LogInformation("LoadLyrics: loading lyrics for {FilePath}", filePath);
        Lyrics.Clear();
        _lyricStates.Clear();
        CurrentLyricIndex = -1;
        OnPropertyChanged(nameof(CurrentLyric));
        HasTranslationAvailable = false;
        HasRomanjiAvailable = false;
        CurrentRomanizationSchema = RomanizationSchema.ErrorOrNotEnabled;
        _currentLyricDuration = songDuration;
        _lastPlaybackTimeSec = 0;
        _currentLyricOffsetMs = 0;
        _currentIntermediateLyricJson = null;
        _suggestedWplrcFileName = GetSuggestedWplrcFileName(filePath, songTitle);
        ExportWplrcCommand.NotifyCanExecuteChanged();

        if (!string.IsNullOrEmpty(suppliedLyric))
        {
            try
            {
                logger.LogInformation("LoadLyrics: found stored or embedded lyrics");
                var loadedLyric = await ParseAndAddLocalLyricAsync(suppliedLyric, offsetMs);
                if (loadedLyric.ShouldWriteBack)
                {
                    RequestCurrentLyricWriteBack(loadedLyric.IntermediateLyricJson);
                }
                else
                {
                    logger.LogInformation("LoadLyrics: loaded current intermediate lyric JSON; skipping write-back");
                }
                return;
            }
            catch
            {
                // ignored - fallback to lrc file read
            }
        }

        var lyricPath = FindBestLyricFile(filePath, songTitle, lyricParserFactory.SupportedOpenExtensions);
        if (!string.IsNullOrEmpty(lyricPath))
        {
            logger.LogInformation("LoadLyrics: found best match lyric file: {LyricPath}", lyricPath);
            try
            {
                var loadedLyric = await ParseAndAddLocalLyricAsync(lyricPath, offsetMs);
                if (loadedLyric.ShouldWriteBack)
                    RequestCurrentLyricWriteBack(loadedLyric.IntermediateLyricJson);
                return;
            }
            catch (Exception ex)
            {
                WpfMessageBox.Show($"Failed to load lyric: {ex.Message}", "Error", WpfMessageBoxIcon.Error);
            }
        }

        var exactLyricPath = FindExactLyricFile(filePath, lyricParserFactory.SupportedOpenExtensions);
        if (exactLyricPath != null)
        {
            logger.LogInformation("LoadLyrics: fallback to exact lyric path: {LyricPath}", exactLyricPath);
            try
            {
                var loadedLyric = await ParseAndAddLocalLyricAsync(exactLyricPath, offsetMs);
                if (loadedLyric.ShouldWriteBack)
                    RequestCurrentLyricWriteBack(loadedLyric.IntermediateLyricJson);
                return;
            }
            catch (InvalidOperationException ex)
            {
                WpfMessageBox.Show(ex.Message, "Error", WpfMessageBoxIcon.Error);
            }
        }

        logger.LogInformation("LoadLyrics: no lyrics found");
        Lyrics.Add(new LyricLineViewModel("暂无歌词"));
    }

    partial void OnCurrentLyricIndexChanged(int value)
    {
        OnPropertyChanged(nameof(CurrentLyric));
    }

    partial void OnCurrentRomanizationSchemaChanged(RomanizationSchema value)
    {
        OnPropertyChanged(nameof(RomanjiButtonContent));
    }

    private double CalculateLinearLyricProgress(int index, double currentTimeSec)
    {
        if (index < 0 || index >= Lyrics.Count) return 0;

        var startMs = Lyrics[index].TimeMs;
        if (startMs < 0) return 0;

        var endMs = (int)Math.Round(_currentLyricDuration * 1000);
        for (var i = index + 1; i < Lyrics.Count; i++)
        {
            if (Lyrics[i].TimeMs <= startMs) continue;
            endMs = Lyrics[i].TimeMs;
            break;
        }

        if (endMs <= startMs) return 1;

        var elapsedMs = currentTimeSec * 1000 - startMs;
        return Math.Clamp(elapsedMs / (endMs - startMs), 0, 1);
    }

    private async Task<LocalLyricLoadResult> ParseAndAddLocalLyricAsync(string content, int offsetMs)
    {
        Lyrics.Clear();
        _lyricStates.Clear();
        _currentIntermediateLyricJson = null;
        CurrentLyricIndex = -1;
        OnPropertyChanged(nameof(CurrentLyric));
        ExportWplrcCommand.NotifyCanExecuteChanged();

        var parseResult = await lyricParserFactory.ParseAsync(
            content,
            songEndTimeMs: (int)Math.Round(_currentLyricDuration * 1000));
        var intermediateLyricJson = parseResult.IntermediateJson;
        var intermediateLyric = IntermediateLyricDocument.FromJson(intermediateLyricJson);

        CurrentRomanizationSchema = intermediateLyric.RomanizationSchema;
        _currentLyricOffsetMs = offsetMs != 0 ? offsetMs : intermediateLyric.Offset;
        if (_currentLyricOffsetMs != intermediateLyric.Offset)
        {
            logger.LogInformation("ParseAndAddLocalLyric: overriding lrc offset to {OffsetMs}", _currentLyricOffsetMs);
        }

        _lyricStates.AddRange(BuildLyricStates(intermediateLyric));
        HasTranslationAvailable = _lyricStates.Any(state => state.ViewModel.HasTranslation);
        HasRomanjiAvailable = _lyricStates.Any(state => state.ViewModel.HasRomanji);

        foreach (var state in _lyricStates)
        {
            Lyrics.Add(state.ViewModel);
        }

        _currentIntermediateLyricJson = intermediateLyricJson;
        ExportWplrcCommand.NotifyCanExecuteChanged();
        return new LocalLyricLoadResult(intermediateLyricJson, parseResult.ShouldWriteBack);
    }

    private void RequestCurrentLyricWriteBack(string intermediateLyricJson)
    {
        UpdateCurrentLyricRequested?.Invoke(intermediateLyricJson, _currentLyricOffsetMs);
    }

    private static string? FindBestLyricFile(
        string? filePath,
        string? songTitle,
        IReadOnlyList<string> supportedLyricExtensions)
    {
        if (string.IsNullOrEmpty(filePath)) return null;

        var fileDir = Path.GetDirectoryName(filePath);
        if (string.IsNullOrEmpty(fileDir)) return null;

        var searchPaths = new List<string>
        {
            fileDir,
            Path.GetFullPath(Path.Combine(fileDir, "..")),
        };

        AddKnownPath(Environment.SpecialFolder.MyMusic);
        AddKnownPath(Environment.SpecialFolder.MyDocuments);

        var targetName = songTitle;
        if (string.IsNullOrEmpty(targetName))
        {
            targetName = Path.GetFileNameWithoutExtension(filePath);
        }

        foreach (var dir in searchPaths.Where(Directory.Exists))
        {
            try
            {
                var lrcFiles = FindLyricFiles(dir, supportedLyricExtensions);
                string? bestFile = null;
                var bestSimilarity = 0f;

                foreach (var file in lrcFiles)
                {
                    var fileName = Path.GetFileNameWithoutExtension(file);

                    if (fileName.Contains(targetName, StringComparison.OrdinalIgnoreCase))
                    {
                        return file;
                    }

                    var sim = CalculateJaccardSimilarity(fileName, targetName);
                    if (!(sim > 0.7f) || !(sim > bestSimilarity)) continue;
                    bestSimilarity = sim;
                    bestFile = file;
                }

                if (bestFile != null) return bestFile;
            }
            catch
            {
                // ignored
            }
        }

        return null;

        void AddKnownPath(Environment.SpecialFolder folder)
        {
            var path = Environment.GetFolderPath(folder);
            if (string.IsNullOrEmpty(path)) return;
            searchPaths.Add(path);
            searchPaths.Add(Path.Combine(path, "Lyrics"));
        }
    }

    private static string? FindExactLyricFile(string? filePath, IReadOnlyList<string> supportedLyricExtensions)
    {
        return string.IsNullOrEmpty(filePath)
            ? null
            : supportedLyricExtensions
                .Select(extension => Path.ChangeExtension(filePath, extension))
                .FirstOrDefault(File.Exists);
    }

    private static string[] FindLyricFiles(string directory, IReadOnlyList<string> supportedLyricExtensions)
    {
        return supportedLyricExtensions
            .SelectMany(extension => Directory.GetFiles(directory, $"*{extension}"))
            .ToArray();
    }

    [RelayCommand]
    private async Task LoadCustomLyrics()
    {
        if (_currentLyricDuration == 0) return;
        var path = await fileDialogService.PickFileAsync(lyricParserFactory.SupportedOpenExtensions);
        if (string.IsNullOrEmpty(path)) return;
        
        try
        {
            var loadedLyric = await ParseAndAddLocalLyricAsync(path, 0);
            UpdateLyricProgress(_lastPlaybackTimeSec);
            if (loadedLyric.ShouldWriteBack)
                RequestCurrentLyricWriteBack(loadedLyric.IntermediateLyricJson);
        }
        catch (InvalidOperationException ex)
        {
            WpfMessageBox.Show(ex.Message, "Error", WpfMessageBoxIcon.Error);
        }
    }

    [RelayCommand(CanExecute = nameof(CanExportWplrc))]
    private async Task ExportWplrc()
    {
        if (!CanExportWplrc() || string.IsNullOrWhiteSpace(_currentIntermediateLyricJson)) return;

        var path = await fileDialogService.SaveFileAsync(
            WplrcSaveFileTypeChoices,
            _suggestedWplrcFileName,
            FileDialogLocation.MusicLibrary);
        if (string.IsNullOrWhiteSpace(path)) return;

        try
        {
            path = EnsureWplrcExtension(path);
            var prettyJson = FormatIntermediateJson(_currentIntermediateLyricJson);
            await File.WriteAllTextAsync(path, prettyJson, Utf8NoBom);
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or JsonException)
        {
            WpfMessageBox.Show($"导出 WPLRC 失败：{ex.Message}", "Error", WpfMessageBoxIcon.Error);
        }
    }

    [RelayCommand]
    public void AdjustLrcOffset()
    {
        if (_lyricStates.Count == 0) return;
        var offsetMs = 
            OffsetAdjustDialog.Show(_currentLyricOffsetMs, 
                title: "调节歌词延迟",
                onChanged: offset =>
                {
                    ApplyLyricOffset(offset);
                    UpdateLyricProgress(_lastPlaybackTimeSec);
                });
        // 合并到确认延迟更新后再写回数据库，避免频繁回写造成性能开销
        UpdateCurrentLyricOffsetRequested?.Invoke(offsetMs);
    }

    private static float CalculateJaccardSimilarity(string str1, string str2)
    {
        var set1 = new HashSet<char>(str1);
        var set2 = new HashSet<char>(str2);

        var intersection = new HashSet<char>(set1);
        intersection.IntersectWith(set2);

        var union = new HashSet<char>(set1);
        union.UnionWith(set2);

        if (union.Count == 0) return 0f;
        return (float)intersection.Count / union.Count;
    }

    private bool CanExportWplrc()
    {
        return _lyricStates.Count > 0 && !string.IsNullOrWhiteSpace(_currentIntermediateLyricJson);
    }

    private static string FormatIntermediateJson(string json)
    {
        using var document = JsonDocument.Parse(json);
        return JsonSerializer.Serialize(document.RootElement, PrettyJsonOptions);
    }

    private static string EnsureWplrcExtension(string path)
    {
        return string.Equals(Path.GetExtension(path), WplrcExtension, StringComparison.OrdinalIgnoreCase)
            ? path
            : Path.ChangeExtension(path, WplrcExtension);
    }

    private static string GetSuggestedWplrcFileName(string? filePath, string? songTitle)
    {
        var candidate = !string.IsNullOrWhiteSpace(songTitle)
            ? songTitle
            : Path.GetFileNameWithoutExtension(filePath);
        if (string.IsNullOrWhiteSpace(candidate))
            return "lyrics";

        var invalidChars = Path.GetInvalidFileNameChars();
        var builder = new StringBuilder(candidate.Length);
        foreach (var ch in candidate)
        {
            builder.Append(invalidChars.Contains(ch) ? '_' : ch);
        }

        var fileName = builder.ToString().Trim();
        return string.IsNullOrWhiteSpace(fileName) ? "lyrics" : fileName;
    }

    private List<LyricState> BuildLyricStates(IntermediateLyricDocument document)
    {
        var states = new List<LyricState>(document.LyricLines.Count);
        foreach (var state in
                 document.LyricLines
                     .Select(BuildLyricState)
                     .OfType<LyricState>())
        {
            AddInterludeStateIfNeeded(states, state);
            states.Add(state);
        }

        return states;
    }

    private LyricState? BuildLyricState(IntermediateLyricLine line)
    {
        var lyricLine = FindRoleLine(line, "lyric") ?? line.Lines.FirstOrDefault();
        if (lyricLine is null) return null;

        var lyricText = GetLineText(lyricLine);
        if (string.IsNullOrWhiteSpace(lyricText)) return null;

        var controllerLine = line.ControllerLineIndex != -1 ? line.Lines[line.ControllerLineIndex] : null;
        var translation = GetOptionalLineText(FindRoleLine(line, "translation"));
        var romanji = GetOptionalLineText(FindRoleLine(line, "romanization"));
        var controllerNodes = BuildControllerNodes(controllerLine);
        var viewModel = new LyricLineViewModel(
            lyricText,
            line.TimeStartMs + _currentLyricOffsetMs,
            translation,
            romanji)
        {
            IsProgressEnabled = controllerNodes is { Count: > 0 }
        };

        return new LyricState(line.TimeStartMs, line.TimeEndMs, controllerNodes, viewModel);
    }

    private void AddInterludeStateIfNeeded(List<LyricState> states, LyricState nextState)
    {
        if (states.Count == 0) return;

        var previousState = states[^1];
        var gapMs = nextState.RawStartTimeMs - previousState.RawEndTimeMs;
        if (previousState.RawEndTimeMs <= previousState.RawStartTimeMs
            || gapMs <= InterludeLyricGapThresholdMs)
        {
            return;
        }

        states.Add(CreateInterludeState(previousState.RawEndTimeMs, nextState.RawStartTimeMs));
    }

    private LyricState CreateInterludeState(int startMs, int endMs)
    {
        var controllerNodes = BuildInterpolatedControllerNodes(startMs, endMs);
        var viewModel = new LyricLineViewModel(InterludeLyricText, startMs + _currentLyricOffsetMs)
        {
            IsProgressEnabled = true
        };

        return new LyricState(startMs, endMs, controllerNodes, viewModel);
    }

    private static IReadOnlyList<LyricControllerNode> BuildInterpolatedControllerNodes(int startMs, int endMs)
    {
        var nodes = new LyricControllerNode[InterludeLyricText.Length];
        var durationMs = endMs - startMs;
        for (var i = 0; i < nodes.Length; i++)
        {
            var nodeStartMs = startMs + (int)Math.Round((double)durationMs * i / nodes.Length);
            var nodeEndMs = i == nodes.Length - 1
                ? endMs
                : startMs + (int)Math.Round((double)durationMs * (i + 1) / nodes.Length);
            nodes[i] = new LyricControllerNode(nodeStartMs, nodeEndMs, InterludeLyricText.Substring(i, 1));
        }

        return nodes;
    }

    private static IntermediateLineNode? FindRoleLine(IntermediateLyricLine line, string role)
    {
        return line.Lines.FirstOrDefault(item => string.Equals(item.Role, role, StringComparison.OrdinalIgnoreCase));
    }

    private static string? GetOptionalLineText(IntermediateLineNode? line)
    {
        if (line is null) return null;

        var text = GetLineText(line);
        return string.IsNullOrWhiteSpace(text) ? null : text;
    }

    private static string GetLineText(IntermediateLineNode line)
    {
        if (line.HasControllerNodesSync && line.ControllerNodes.Count > 0)
            return string.Concat(line.ControllerNodes.Select(node => node.Text ?? string.Empty));

        return line.Text ?? string.Empty;
    }

    private static IReadOnlyList<LyricControllerNode>? BuildControllerNodes(IntermediateLineNode? line)
    {
        if (line is null) return null;
        if (!line.HasControllerNodesSync) return null;

        var controllerNodes = line.ControllerNodes
            .Select(node => new LyricControllerNode(node.TimeStartMs, node.TimeEndMs, node.Text ?? string.Empty))
            .Where(node => !string.IsNullOrEmpty(node.Text))
            .ToArray();

        return controllerNodes.Length == 0 ? null : controllerNodes;
    }

    private void ApplyLyricOffset(int offsetMs)
    {
        _currentLyricOffsetMs = offsetMs;
        foreach (var state in _lyricStates)
        {
            state.ViewModel.UpdateTimeMs(state.RawStartTimeMs + _currentLyricOffsetMs);
        }
    }

    private int GetLyricIndexAtRawTime(int rawTimeMs)
    {
        if (_lyricStates.Count == 0) return -1;
        if (rawTimeMs < _lyricStates[0].RawStartTimeMs)
            return 0;

        if (CurrentLyricIndex >= 0 && CurrentLyricIndex < _lyricStates.Count)
        {
            var nextIndex = CurrentLyricIndex + 1;
            var current = _lyricStates[CurrentLyricIndex];
            if (rawTimeMs >= current.RawStartTimeMs
                && (nextIndex >= _lyricStates.Count || rawTimeMs < _lyricStates[nextIndex].RawStartTimeMs))
            {
                return CurrentLyricIndex;
            }
        }

        var left = 0;
        var right = _lyricStates.Count - 1;
        var result = 0;
        while (left <= right)
        {
            var mid = left + (right - left) / 2;
            if (_lyricStates[mid].RawStartTimeMs <= rawTimeMs)
            {
                result = mid;
                left = mid + 1;
            }
            else
            {
                right = mid - 1;
            }
        }

        return result;
    }

    private static double CalculateControllerLyricProgress(LyricState state, int rawTimeMs)
    {
        var controllerNodes = state.ControllerNodes;
        if (controllerNodes is not { Count: > 0 }) return 1;
        if (rawTimeMs < state.RawStartTimeMs) return 0;
        if (rawTimeMs >= controllerNodes[^1].RawEndTimeMs) return 1;

        for (var i = 0; i < controllerNodes.Count; ++i)
        {
            var node = controllerNodes[i];
            if (rawTimeMs >= node.RawEndTimeMs) continue;

            var nodeProgress = node.RawEndTimeMs > node.RawStartTimeMs
                ? (double)(rawTimeMs - node.RawStartTimeMs) / (node.RawEndTimeMs - node.RawStartTimeMs)
                : 1;
            nodeProgress = Math.Clamp(nodeProgress, 0, 1);
            return Math.Clamp((i + nodeProgress) / controllerNodes.Count, 0, 1);
        }

        return 1;
    }

    private sealed record LyricState(
        int RawStartTimeMs,
        int RawEndTimeMs,
        IReadOnlyList<LyricControllerNode>? ControllerNodes,
        LyricLineViewModel ViewModel);

    private sealed record LocalLyricLoadResult(string IntermediateLyricJson, bool ShouldWriteBack);

    private sealed record LyricControllerNode(int RawStartTimeMs, int RawEndTimeMs, string Text);
    
}
