// SPDX-License-Identifier: MIT

using System.Windows.Media.Imaging;
using CommunityToolkit.Mvvm.ComponentModel;

namespace WpfMusicPlayer.ViewModels;

public partial class PlaylistItemViewModel(string filePath, string title, string artist, int playedCount) : ObservableObject
{
    public string FilePath { get; } = filePath;
    
    [ObservableProperty]
    public partial string Title { get; set; } = title;
    
    [ObservableProperty]
    public partial string Artist { get; set; } = artist;
    
    // 删除duration，改为播放次数
    [ObservableProperty]
    public partial int PlayedCount { get; set; } = playedCount;

    [ObservableProperty]
    public partial BitmapImage? AlbumCover { get; set; }

    [ObservableProperty]
    public partial bool IsPlaying { get; set; }
}
