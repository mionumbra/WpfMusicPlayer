// SPDX-License-Identifier: MIT

namespace WpfMusicPlayer.Models;

public class SongRecord
{
    public int Id { get; set; }
    public string Md5 { get; set; } = string.Empty;
    public byte[]? AlbumArt { get; set; }
    public string Artist { get; set; } = string.Empty;
    public string Title { get; set; } = string.Empty;
    public int PlayCount { get; set; }
    public string CustomLyric { get; set; } = string.Empty;
    public int CustomLyricOffsetMs { get; set; }
}
