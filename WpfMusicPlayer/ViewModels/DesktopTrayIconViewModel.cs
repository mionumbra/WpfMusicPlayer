// SPDX-License-Identifier: MIT

using System.Windows;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using Microsoft.Extensions.Logging;

namespace WpfMusicPlayer.ViewModels;

public partial class DesktopTrayIconViewModel
    : ObservableObject
{
    private const string DesktopLyricVisibleOnTooltip = "隐藏桌面歌词";
    private const string DesktopLyricVisibleOffTooltip = "显示桌面歌词";
    private DesktopLyricViewModel _desktopLyricViewModel;
    private ILogger<DesktopTrayIconViewModel> _logger;

    public DesktopTrayIconViewModel(DesktopLyricViewModel desktopLyricViewModel, ILogger<DesktopTrayIconViewModel> logger)
    {
        _logger = logger;
        _desktopLyricViewModel = desktopLyricViewModel;
        _desktopLyricViewModel.PropertyChanged += (s, e) =>
        {
            switch (e.PropertyName)
            {
                case nameof(DesktopLyricViewModel.IsDesktopLyricVisible):
                    OnDesktopLyricVisibleChanged();
                    break;
            }
        };
    }
    
    [ObservableProperty]
    public partial Visibility IsTaskbarIconVisible { get; set; } = Visibility.Collapsed;

    [ObservableProperty]
    public partial string TaskBarToolTipText { get; set; } = "WpfMusicPlayer";

    [ObservableProperty]
    public partial string DesktopLyricVisibleTooltip { get; set; } = DesktopLyricVisibleOffTooltip;

    public void EnableTaskbarIcon()
    {
        IsTaskbarIconVisible = Visibility.Visible;
    }

    public void DisableTaskbarIcon()
    {
        IsTaskbarIconVisible = Visibility.Collapsed;
    }

    [RelayCommand]
    public void ToggleMainWindow()
    {
        _logger.LogInformation("Toggling main window visibility");
        var mainWindow = Application.Current.MainWindow;
        if (mainWindow == null)
            return;

        mainWindow.Show();
        if (mainWindow.WindowState == WindowState.Minimized)
            mainWindow.WindowState = WindowState.Normal;
        mainWindow.Activate();
        DisableTaskbarIcon();
    }

    public void OnDesktopLyricVisibleChanged()
    {
        DesktopLyricVisibleTooltip = _desktopLyricViewModel.IsDesktopLyricVisible
            ? DesktopLyricVisibleOnTooltip
            : DesktopLyricVisibleOffTooltip;
    }

    [RelayCommand]
    public void ToggleDesktopLyric()
    {
        _desktopLyricViewModel.IsDesktopLyricVisible = !_desktopLyricViewModel.IsDesktopLyricVisible;
    }

    [RelayCommand]
    public void ShutdownApplication()
    {
        Application.Current.Shutdown();
    }
}
