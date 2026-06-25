using System.Diagnostics;
using System.Globalization;
using System.Linq;
using System.Windows;
using WpfMusicPlayer.ViewModels;

namespace WpfMusicPlayer.Helpers;

internal static class RebootApplicationHelper
{
    public static void RebootApplication()
    {
        if (Application.Current.MainWindow?.DataContext is not MainViewModel vm)
            return;

        var args = BuildCommandLineArgs(vm);
        var exePath = Environment.ProcessPath;

        if (exePath is not null)
        {
            Process.Start(exePath, args);
        }

        Application.Current.Shutdown();
    }

    private static string BuildCommandLineArgs(MainViewModel vm)
    {
        var parts = new List<string>();

        var currentItem = vm.Playlist.PlaylistItems.FirstOrDefault(p => p.IsPlaying);
        if (currentItem is not null)
        {
            parts.Add("--file");
            parts.Add(Quote(currentItem.FilePath));
            parts.Add("--time");
            parts.Add(((float)vm.ProgressValue).ToString(CultureInfo.InvariantCulture));
            if (vm.IsMusicPlaying)
            {
                parts.Add("--autostart");
                parts.Add("true");
            }
        }
        parts.Add("--view");
        parts.Add(vm.ActiveView.ToString());
        // 预留的接口
        // if (!string.IsNullOrEmpty(vm.OpenedPlaylistPath))
        // {
        //     parts.Add("--playlist");
        //     parts.Add(Quote(vm.OpenedPlaylistPath));
        // }
        if (vm.Lyrics.IsTranslationVisible)
        {
            parts.Add("--translation");
            parts.Add("true");
        }
        if (vm.Lyrics.IsRomanjiVisible)
        {
            parts.Add("--romanji");
            parts.Add("true");
        }
        parts.Add("--playmode");
        parts.Add(vm.CurrentPlayMode.ToString());
        parts.Add("--desktoplyric");
        parts.Add(vm.DesktopLyric.IsDesktopLyricVisible.ToString());
        // 注意: Microsoft.Extensions.Configuration.Logging以子键方式索引数组
        // 例如: --eq:0 1 --eq:1 3 --eq:2 5
        var bands = vm.Equalizer.Bands;
        for (var i = 0; i < bands.Count; i++)
        {
            if (!bands[i].IsEnabled) continue;
            parts.Add($"--eq:{i}");
            parts.Add(bands[i].Value.ToString(CultureInfo.InvariantCulture));
        }

        var result = string.Join(" ", parts);
        return result;
    }
    
    private static string Quote(string value)
        => value.Contains(' ') ? $"\"{value}\"" : value;
}
