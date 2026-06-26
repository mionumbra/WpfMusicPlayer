// SPDX-License-Identifier: MIT

using System.Windows;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using Microsoft.Extensions.Logging;

namespace WpfMusicPlayer.ViewModels;

public partial class DesktopLyricViewModel
    : ObservableObject
{
    private readonly LyricsViewModel _lyricsVm;
    private ILogger<DesktopLyricViewModel> _logger;

    public DesktopLyricViewModel(LyricsViewModel lyricsVm, ILogger<DesktopLyricViewModel> logger)
    {
        _lyricsVm = lyricsVm;
        _logger = logger;

        _lyricsVm.PropertyChanged += (_, e) =>
        {
            switch (e.PropertyName)
            {
                case nameof(LyricsViewModel.CurrentLyricIndex):
                    OnPropertyChanged(nameof(CurrentLyric));
                    OnPropertyChanged(nameof(TranslationVisibility));
                    OnPropertyChanged(nameof(RomanjiVisibility));
                    break;
                case nameof(LyricsViewModel.IsTranslationVisible):
                    OnPropertyChanged(nameof(TranslationVisibility));
                    break;
                case nameof(LyricsViewModel.IsRomanjiVisible):
                    OnPropertyChanged(nameof(RomanjiVisibility));
                    break;
                case nameof(LyricsViewModel.HasTranslationAvailable):
                    OnPropertyChanged(nameof(HasTranslation));
                    OnPropertyChanged(nameof(TranslationVisibility));
                    break;
                case nameof(LyricsViewModel.HasRomanjiAvailable):
                    OnPropertyChanged(nameof(HasRomanji));
                    OnPropertyChanged(nameof(RomanjiVisibility));
                    break;
            }
        };
    }

    public LyricLineViewModel CurrentLyric
    {
        get
        {
            var idx = _lyricsVm.CurrentLyricIndex;
            if (idx >= 0 && idx < _lyricsVm.Lyrics.Count)
                return _lyricsVm.Lyrics[idx];
            return new LyricLineViewModel("暂无歌词");
        }
    }

    public bool HasTranslation =>
        _lyricsVm.HasTranslationAvailable;
    
    public bool HasRomanji =>
        _lyricsVm.HasRomanjiAvailable;
    
    public bool IsTranslationOn 
    {
        get;
        set
        {
            if (!SetProperty(ref field, value)) return;
            OnPropertyChanged(nameof(TranslationVisibility));
            _logger.LogInformation("Translation visibility toggled");
        } 
    } = true;

    public bool IsRomanjiOn
    {
        get;
        set
        {
            if (!SetProperty(ref field, value)) return;
            OnPropertyChanged(nameof(RomanjiVisibility));
            _logger.LogInformation("Romanji visibility toggled");
        }
    } = true;
    
    [RelayCommand]
    private void ToggleTranslation() => IsTranslationOn = !IsTranslationOn;

    [RelayCommand]
    private void ToggleRomanji() => IsRomanjiOn = !IsRomanjiOn;

    public Visibility TranslationVisibility =>
        CurrentLyric.HasTranslation && IsTranslationOn
            ? Visibility.Visible
            : Visibility.Collapsed;

    public Visibility RomanjiVisibility =>
        CurrentLyric.HasRomanji && IsRomanjiOn
            ? Visibility.Visible
            : Visibility.Collapsed;

    [ObservableProperty]
    public partial double FontSize { get; set; } = 24;

    [ObservableProperty] 
    public partial double AuxFontSize { get; set; } = 16;

    [ObservableProperty]
    public partial bool IsAuxInfoCustomizable { get; set; }= false;

    partial void OnFontSizeChanged(double value)
    {
        if (!IsAuxInfoCustomizable)
            AuxFontSize = value * 2.0 / 3.0;
    }

    public void CustomizeAuxInfoFontSize(double value)
    {
        IsAuxInfoCustomizable = true;
        AuxFontSize = value;
    }

    public void DiscustomizeAuxInfoFontSize()
    {
        IsAuxInfoCustomizable = false;
    }

    [ObservableProperty]
    public partial bool IsDesktopLyricVisible { get; set; } = false;
}
