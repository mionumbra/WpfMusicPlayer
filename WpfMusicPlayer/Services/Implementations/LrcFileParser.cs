// SPDX-License-Identifier: MIT

using System.IO;
using System.Text.RegularExpressions;
using MusicPlayerLibrary;
using WpfMusicPlayer.Services.Abstractions;

namespace WpfMusicPlayer.Services.Implementations;

public sealed partial class LrcFileParser : ILyricParser
{
    private const string Extension = ".lrc";

    public IReadOnlyList<string> SupportedOpenExtensions { get; } = [Extension];

    public bool CanParse(LyricParserSource source)
    {
        if (string.Equals(source.Extension, Extension, StringComparison.OrdinalIgnoreCase))
            return true;

        return LrcTimestampRegex().IsMatch(ReadLrcText(source));
    }

    public string ParseToIntermediateJson(LyricParserSource source)
    {
        var lrc = ReadLrcText(source);
        using var controller = new LrcFileController(source.SongEndTimeMs);
        return controller.ParseLrcToIntermediateJson(lrc);
    }

    public LyricParseResult Parse(LyricParserSource source)
    {
        return new LyricParseResult(ParseToIntermediateJson(source), ShouldWriteBack: true);
    }

    public async Task<string> ParseToIntermediateJsonAsync(LyricParserSource source)
    {
        return await Task.Run(() => ParseToIntermediateJson(source));
    }

    public async Task<LyricParseResult> ParseAsync(LyricParserSource source)
    {
        return await Task.Run(() => Parse(source));
    }

    private static string ReadLrcText(LyricParserSource source)
    {
        var path = source.EffectiveFilePath;
        if (path is null)
            return source.Input;

        var bytes = File.ReadAllBytes(path);
        return LocaleConverter.GetSystemStringFromBytes(bytes);
    }

    [GeneratedRegex(@"\[\s*\d{1,2}\s*[:.]\s*\d{1,2}(?:\s*[:.]\s*\d{1,4})?\s*\]")]
    private static partial Regex LrcTimestampRegex();
}
