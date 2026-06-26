// SPDX-License-Identifier: MIT

namespace WpfMusicPlayer.Services.Abstractions;

public interface ILyricParser
{
    IReadOnlyList<string> SupportedOpenExtensions { get; }

    public bool CanParse(LyricParserSource source);
    
    string ParseToIntermediateJson(LyricParserSource source);
}
