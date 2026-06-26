// SPDX-License-Identifier: MIT

using WpfMusicPlayer.ViewModels;

namespace WpfMusicPlayer.Services.Abstractions;

public interface ICommandLineParser
{
    public string FilePath { get; }

    public float MusicCurrentTime { get; }

    public bool AutoStart { get; }

    public ActiveView StartupView { get; }

    public string OpenedPlaylistPath { get; }

    public bool TranslationToggled { get; }

    public bool RomanjiToggled { get; }
    
    public PlayMode CurrentPlayMode { get; }
    
    public bool IsDesktopLyricToggled { get; }

    public int[] AppliedEqualizerSettings { get; }
}
