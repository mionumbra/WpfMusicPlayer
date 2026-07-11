// SPDX-License-Identifier: MIT

using System.IO;
using System.Text;
using MusicPlayerLibrary;
using WpfMusicPlayer.Models.Lyrics;
using WpfMusicPlayer.Services.Abstractions;

namespace WpfMusicPlayer.Services.Implementations;

public sealed class IntermediateLyricParser : ILyricParser
{
    private const string Extension = ".wplrc";
    private static readonly byte[] Utf8Bom = [0xEF, 0xBB, 0xBF];
    private static readonly Encoding StrictUtf8 = new UTF8Encoding(
        encoderShouldEmitUTF8Identifier: false,
        throwOnInvalidBytes: true);

    public IReadOnlyList<string> SupportedOpenExtensions { get; } = [Extension];

    public bool CanParse(LyricParserSource source)
    {
        if (string.Equals(source.Extension, Extension, StringComparison.OrdinalIgnoreCase))
            return true;

        try
        {
            var text = ReadIntermediateText(source);
            return IntermediateLyricDocument.HasIntermediateSchema(text);
        }
        catch (InvalidOperationException)
        {
            return false;
        }
    }

    public string ParseToIntermediateJson(LyricParserSource source)
    {
        return Parse(source).IntermediateJson;
    }

    public LyricParseResult Parse(LyricParserSource source)
    {
        var json = ReadIntermediateText(source);
        var normalized = IntermediateLyricDocument.NormalizeToCurrentVersion(json);
        _ = IntermediateLyricDocument.FromJson(normalized.Json);
        return new LyricParseResult(normalized.Json, normalized.WasUpgraded);
    }

    public async Task<string> ParseToIntermediateJsonAsync(LyricParserSource source)
    {
        return await Task.Run(() => ParseToIntermediateJson(source));
    }

    public async Task<LyricParseResult> ParseAsync(LyricParserSource source)
    {
        return await Task.Run(() => Parse(source));
    }

    private static string ReadIntermediateText(LyricParserSource source)
    {
        var path = source.EffectiveFilePath;
        if (path is null)
            return source.Input;

        var bytes = File.ReadAllBytes(path);
        if (!LocaleConverterManaged.IsUtf8CompatibleBytesManaged(bytes))
            throw new InvalidOperationException("WPLRC files must be UTF-8 encoded.");

        try
        {
            var span = bytes.AsSpan();
            if (span.StartsWith(Utf8Bom))
                span = span[Utf8Bom.Length..];

            return StrictUtf8.GetString(span);
        }
        catch (DecoderFallbackException ex)
        {
            throw new InvalidOperationException("WPLRC files must be UTF-8 encoded.", ex);
        }
    }
}
