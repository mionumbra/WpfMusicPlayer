// SPDX-License-Identifier: MIT

using System.IO;

namespace WpfMusicPlayer.Services.Abstractions;

public enum PlaybackState
{
    Playing,
    Paused,
    Stopped,
    Closed
}

public interface ISmtcService : IDisposable
{
    event Action PlayRequested;
    event Action PauseRequested;
    event Action NextRequested;
    event Action PreviousRequested;

    void Initialize(IntPtr handle);
    void UpdateMetadata(string title, string artist, Stream? albumArtStream);
    void UpdateTextMetadata(string title, string artist);
    void UpdatePlaybackStatus(PlaybackState state);
}

