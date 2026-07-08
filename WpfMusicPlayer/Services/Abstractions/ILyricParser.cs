// SPDX-License-Identifier: MIT

namespace WpfMusicPlayer.Services.Abstractions;

public sealed record LyricParseResult(string IntermediateJson, bool ShouldWriteBack);

public interface ILyricParser
{
    IReadOnlyList<string> SupportedOpenExtensions { get; }

    public bool CanParse(LyricParserSource source);

    LyricParseResult Parse(LyricParserSource source);
    Task<LyricParseResult> ParseAsync(LyricParserSource source);

    string ParseToIntermediateJson(LyricParserSource source);
    Task<string> ParseToIntermediateJsonAsync(LyricParserSource source);
}
