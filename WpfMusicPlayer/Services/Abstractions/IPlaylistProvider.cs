// SPDX-License-Identifier: MIT

using WpfMusicPlayer.Models;

namespace WpfMusicPlayer.Services.Abstractions;

public interface IPlaylistProvider
{
    public enum ErrorCode
    {
        NoError,
        FileNotFound,
        PermissionDenied,
        FileOpenFailed,
        ParseError,
        UnknownError
    }

    string? CurrentFilePath { get; }

    ErrorCode Load(string filePath);

    ErrorCode Save(string filePath);
    
    ErrorCode CreateDefault();

    ref PlaylistRecord GetPlaylist();
}