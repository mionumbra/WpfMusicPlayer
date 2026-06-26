// SPDX-License-Identifier: MIT

using System.Windows;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;

namespace WpfMusicPlayer.ViewModels;

public partial class LyricLineViewModel(string text, int timeMs = -1, string? translation = null, string? romanji = null) : ObservableObject
{
    public string Text { get; } = text;

    public int TimeMs { get; private set; } = timeMs;

    public string? Translation { get; } = translation;

    public string? Romanji { get; } = romanji;

    public bool HasTranslation => Translation != null;

    public bool HasRomanji => Romanji != null;

    [ObservableProperty]
    public partial bool IsHighlighted { get; set; }

    public bool IsProgressEnabled { get; init; }

    [ObservableProperty]
    public partial double Progress { get; set; }

    public void UpdateTimeMs(int timeMs)
    {
        if (TimeMs == timeMs) return;
        TimeMs = timeMs;
        OnPropertyChanged(nameof(TimeMs));
    }

    [RelayCommand]
    public void CopyLyricText()
    {
        Clipboard.SetText(Text);
    }

    [RelayCommand]
    public void CopyLyricTranslation()
    {
        if (Translation != null)
            Clipboard.SetText(Translation);
    }

    [RelayCommand]
    public void CopyLyricRomanji()
    {
        if (Romanji != null)
            Clipboard.SetText(Romanji);
    }
}

