// SPDX-License-Identifier: MIT

using WpfMusicPlayer.Services.Abstractions;

namespace WpfMusicPlayer.Services.Implementations;

public sealed class LyricParserFactory
{
    private readonly IReadOnlyList<ILyricParser> _parsers;

    public LyricParserFactory(IEnumerable<ILyricParser> parsers)
    {
        _parsers = parsers.ToArray();
        SupportedOpenExtensions = _parsers
            .SelectMany(parser => parser.SupportedOpenExtensions)
            .Select(NormalizeExtension)
            .Where(extension => !string.IsNullOrEmpty(extension))
            .Distinct(StringComparer.OrdinalIgnoreCase)
            .ToArray();
    }

    public IReadOnlyList<string> SupportedOpenExtensions { get; }

    public string ParseToIntermediateJson(string input, string? sourcePath = null)
    {
        var source = new LyricParserSource(input, sourcePath);
        var parser = _parsers.FirstOrDefault(candidate => candidate.CanParse(source))
            ?? throw new InvalidOperationException("Unsupported lyric format.");

        return parser.ParseToIntermediateJson(source);
    }

    private static string NormalizeExtension(string extension)
    {
        if (string.IsNullOrWhiteSpace(extension))
            return string.Empty;

        extension = extension.Trim();
        return extension.StartsWith('.') ? extension : $".{extension}";
    }
}
