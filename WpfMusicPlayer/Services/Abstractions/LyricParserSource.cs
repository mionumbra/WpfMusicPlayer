// SPDX-License-Identifier: MIT

using System.IO;
using System.Text;

namespace WpfMusicPlayer.Services.Abstractions;

public sealed class LyricParserSource
{
    public LyricParserSource(string input, string? sourcePath = null, int songEndTimeMs = 0)
    {
        Input = input;
        SourcePath = sourcePath;
        SongEndTimeMs = songEndTimeMs;
    }

    public string Input { get; }

    public string? SourcePath { get; }

    public int SongEndTimeMs { get; }

    public string? EffectiveFilePath
    {
        get
        {
            if (!string.IsNullOrWhiteSpace(SourcePath))
                return SourcePath;

            return File.Exists(Input) ? Input : null;
        }
    }

    public string? Extension
    {
        get
        {
            var path = EffectiveFilePath;
            return string.IsNullOrEmpty(path) ? null : Path.GetExtension(path);
        }
    }

    public string ReadTextAsUtf8()
    {
        var path = EffectiveFilePath;
        return path is null ? Input : File.ReadAllText(path, Encoding.UTF8);
    }

}
