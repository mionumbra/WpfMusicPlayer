// SPDX-License-Identifier: MIT

using WpfMusicPlayer.Models.Lyrics;
using WpfMusicPlayer.Services.Abstractions;

namespace WpfMusicPlayer.Services.Implementations;

public sealed class IntermediateLyricParser : ILyricParser
{
    private const string Extension = ".wplrc";

    public IReadOnlyList<string> SupportedOpenExtensions { get; } = [Extension];

    public bool CanParse(LyricParserSource source)
    {
        if (string.Equals(source.Extension, Extension, StringComparison.OrdinalIgnoreCase))
            return true;

        var text = source.ReadTextAsUtf8();
        return IntermediateLyricDocument.HasIntermediateSchema(text);
    }

    public string ParseToIntermediateJson(LyricParserSource source)
    {
        var json = source.ReadTextAsUtf8();
        _ = IntermediateLyricDocument.FromJson(json);
        return json;
    }
}
