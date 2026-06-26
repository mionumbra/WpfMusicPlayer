// SPDX-License-Identifier: MIT

using System.Windows;
using Windows.Storage.Pickers;
using WinRT.Interop;
using WpfMusicPlayer.Services.Abstractions;

namespace WpfMusicPlayer.Services.Implementations;

public class FileDialogService : IFileDialogService
{
    private static readonly IReadOnlyList<string> ImageFileExtensions = [
        ".bmp", ".jpg", ".jpeg", ".png", ".tiff", ".webp", ".ico", ".gif"
    ];

    private static readonly IReadOnlyList<string> PlaylistFileExtensions = [".wppl"];

    private static readonly IReadOnlyDictionary<string, IReadOnlyList<string>> PlaylistSaveFileTypeChoices =
        new Dictionary<string, IReadOnlyList<string>>
        {
            ["WpfMusicPlayer 播放列表"] = PlaylistFileExtensions
        };

    public IReadOnlyList<string> MusicFileExtensions { get; } = [
        ".mp3", ".flac", ".wav", ".wma", ".m4a", ".m4s", ".aac", ".ogg", ".ape", ".aiff", ".au", ".aifc",
        ".ncm"
    ];

    public Task<string?> PickMusicFileAsync() =>
        PickFileAsync(MusicFileExtensions, FileDialogLocation.MusicLibrary, FileDialogViewMode.Thumbnail);

    public Task<IReadOnlyList<string>> PickMusicFilesAsync() =>
        PickFilesAsync(MusicFileExtensions, FileDialogLocation.MusicLibrary, FileDialogViewMode.Thumbnail);

    public Task<string?> PickImageAsync() =>
        PickFileAsync(ImageFileExtensions, FileDialogLocation.PicturesLibrary, FileDialogViewMode.Thumbnail);

    public Task<string?> PickWpplAsync() =>
        PickFileAsync(PlaylistFileExtensions);

    public Task<string?> SaveWpplAsync(string suggestedFileName = "playlist") =>
        SaveFileAsync(PlaylistSaveFileTypeChoices, suggestedFileName);

    public async Task<string?> PickFileAsync(
        IReadOnlyList<string> extensions,
        FileDialogLocation suggestedStartLocation = FileDialogLocation.DocumentsLibrary,
        FileDialogViewMode viewMode = FileDialogViewMode.List)
    {
        var picker = CreateOpenPicker(extensions, suggestedStartLocation, viewMode);
        var file = await picker.PickSingleFileAsync();
        return file?.Path;
    }

    public async Task<IReadOnlyList<string>> PickFilesAsync(
        IReadOnlyList<string> extensions,
        FileDialogLocation suggestedStartLocation = FileDialogLocation.DocumentsLibrary,
        FileDialogViewMode viewMode = FileDialogViewMode.List)
    {
        var picker = CreateOpenPicker(extensions, suggestedStartLocation, viewMode);
        var files = await picker.PickMultipleFilesAsync();
        return files?.Select(file => file.Path).ToList() ?? [];
    }

    public async Task<string?> SaveFileAsync(
        IReadOnlyDictionary<string, IReadOnlyList<string>> fileTypeChoices,
        string suggestedFileName,
        FileDialogLocation suggestedStartLocation = FileDialogLocation.DocumentsLibrary)
    {
        ArgumentNullException.ThrowIfNull(fileTypeChoices);

        if (fileTypeChoices.Count == 0)
            throw new ArgumentException("At least one file type choice is required.", nameof(fileTypeChoices));

        var picker = new FileSavePicker
        {
            SuggestedStartLocation = ToPickerLocationId(suggestedStartLocation),
            SuggestedFileName = suggestedFileName
        };

        foreach (var (description, extensions) in fileTypeChoices)
        {
            picker.FileTypeChoices.Add(description, NormalizeExtensions(extensions).ToList());
        }

        InitializeWithMainWindow(picker);

        var file = await picker.PickSaveFileAsync();
        return file?.Path;
    }

    private static FileOpenPicker CreateOpenPicker(
        IReadOnlyList<string> extensions,
        FileDialogLocation suggestedStartLocation,
        FileDialogViewMode viewMode)
    {
        var picker = new FileOpenPicker
        {
            ViewMode = ToPickerViewMode(viewMode),
            SuggestedStartLocation = ToPickerLocationId(suggestedStartLocation)
        };

        foreach (var extension in NormalizeExtensions(extensions))
        {
            picker.FileTypeFilter.Add(extension);
        }

        InitializeWithMainWindow(picker);
        return picker;
    }

    private static IReadOnlyList<string> NormalizeExtensions(IEnumerable<string> extensions)
    {
        ArgumentNullException.ThrowIfNull(extensions);

        var normalized = extensions
            .Where(extension => !string.IsNullOrWhiteSpace(extension))
            .Select(extension =>
            {
                extension = extension.Trim();
                return extension == "*" || extension.StartsWith('.') ? extension : $".{extension}";
            })
            .Distinct(StringComparer.OrdinalIgnoreCase)
            .ToArray();

        return normalized.Length == 0 
            ? throw new ArgumentException("At least one file extension is required.", nameof(extensions)) 
            : normalized;
    }

    private static PickerLocationId ToPickerLocationId(FileDialogLocation location) =>
        location switch
        {
            FileDialogLocation.DocumentsLibrary => PickerLocationId.DocumentsLibrary,
            FileDialogLocation.MusicLibrary => PickerLocationId.MusicLibrary,
            FileDialogLocation.PicturesLibrary => PickerLocationId.PicturesLibrary,
            FileDialogLocation.VideosLibrary => PickerLocationId.VideosLibrary,
            FileDialogLocation.Desktop => PickerLocationId.Desktop,
            FileDialogLocation.Downloads => PickerLocationId.Downloads,
            FileDialogLocation.ComputerFolder => PickerLocationId.ComputerFolder,
            FileDialogLocation.HomeGroup => PickerLocationId.HomeGroup,
            FileDialogLocation.Objects3D => PickerLocationId.Objects3D,
            FileDialogLocation.Unspecified => PickerLocationId.Unspecified,
            _ => PickerLocationId.DocumentsLibrary
        };

    private static PickerViewMode ToPickerViewMode(FileDialogViewMode viewMode) =>
        viewMode switch
        {
            FileDialogViewMode.Thumbnail => PickerViewMode.Thumbnail,
            _ => PickerViewMode.List
        };

    private static void InitializeWithMainWindow(object picker)
    {
        var hwnd = new System.Windows.Interop.WindowInteropHelper(
            Application.Current.MainWindow ?? throw new InvalidOperationException()).Handle;
        InitializeWithWindow.Initialize(picker, hwnd);
    }
}
