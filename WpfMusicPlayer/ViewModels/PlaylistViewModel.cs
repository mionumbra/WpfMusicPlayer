// SPDX-License-Identifier: MIT

using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using Microsoft.Extensions.Logging;
using System.Collections.ObjectModel;
using System.IO;
using System.Security.Cryptography;
using System.Windows.Media.Imaging;
using Windows.Media.Playlists;
using WpfMusicPlayer.Helpers;
using WpfMusicPlayer.Models;
using WpfMusicPlayer.Services.Abstractions;

namespace WpfMusicPlayer.ViewModels;

public partial class PlaylistViewModel(
    IFileDialogService fileDialogService,
    ISongDatabaseService songDatabase,
    IPlaylistProvider playlistProvider,
    ILogger<PlaylistViewModel> logger)
    : ObservableObject
{
    private bool _isPlaylistUserOpened;
    private bool _isPlaylistDirty;
    private bool _isFileDialogOpen;

    public event Action<string, bool>? PlaySongRequested;
    public event Action? ResetPlaylistRequested;
    public event Action<string>? RemovePlaylistItemRequested;

    public ObservableCollection<PlaylistItemViewModel> PlaylistItems { get; } = [];

    public string PlaylistTitle
    {
        get;
        set
        {
            if (!SetProperty(ref field, value)) return;
            playlistProvider.GetPlaylist().Name = value;
            logger.LogInformation("User changed playlist title, mark playlist as dirty");
            _isPlaylistUserOpened = true;
            _isPlaylistDirty = true;
        }
    } = "播放列表";

    [ObservableProperty]
    public partial BitmapImage? PlaylistCoverImage { get; private set; }

    public bool HasUnsavedPlaylistChanges => _isPlaylistUserOpened && _isPlaylistDirty;

    public Func<bool>? IsMusicPlayingQuery { get; set; }

    [RelayCommand]
    public async Task OpenPlaylistAsync(string? filePath = null)
    {
        logger.LogInformation("OpenPlaylistAsync: opening playlist");
        if (HasUnsavedPlaylistChanges)
        {
            var result = WpfMessageBox.Show(
                "播放列表有未保存的更改，是否保存？",
                "确认",
                WpfMessageBoxButton.YesNoCancel,
                WpfMessageBoxIcon.Question);

            switch (result)
            {
                case WpfMessageBoxResult.Yes:
                    await SavePlaylistAsync();
                    break;
                case WpfMessageBoxResult.Cancel:
                    return;
            }
        }
        if (_isFileDialogOpen) return;
        _isFileDialogOpen = true;
        string path;
        if (string.IsNullOrEmpty(filePath))
            path = await fileDialogService.PickWpplAsync() ?? string.Empty;
        else 
            path = filePath;
        _isFileDialogOpen = false;

        if (string.IsNullOrEmpty(path)) return;
        try
        {
            logger.LogInformation("OpenPlaylistAsync: loading playlist from {Path}", path);
            var errorCode = playlistProvider.Load(path);
            if (errorCode != IPlaylistProvider.ErrorCode.NoError)
            {
                logger.LogWarning("OpenPlaylistAsync: failed to load playlist, error: {ErrorCode}", errorCode);
                WpfMessageBox.Show($"加载播放列表失败: {errorCode}", "Error", WpfMessageBoxIcon.Error);
                return;
            }

            var playlist = playlistProvider.GetPlaylist();

            if (string.IsNullOrEmpty(playlist.Id))
            {
                WpfMessageBox.Show("播放列表格式错误: 缺少 ID", "Error", WpfMessageBoxIcon.Error);
                return;
            }

            if (playlist.FormatVersion != 3)
            {
                WpfMessageBox.Show($"播放列表格式版本不支持: {playlist.FormatVersion}，需要版本 3", "Error", WpfMessageBoxIcon.Error);
                return;
            }

            PlaylistItems.Clear();
            PlaylistTitle = string.IsNullOrEmpty(playlist.Name) ? "播放列表" : playlist.Name;
            PlaylistCoverImage = TryLoadPlaylistCover(playlist.Cover);

            foreach (var content in playlist.Contents)
            {
                if (!File.Exists(content.File)) continue;
                var cached = songDatabase.FindByMd5(content.Md5);
                var title = cached?.Title ?? Path.GetFileNameWithoutExtension(content.File);
                var artist = cached?.Artist ?? "Unknown Artist";
                var playedCount = cached?.PlayCount ?? 0;
                var itemVM = new PlaylistItemViewModel(content.File, title, artist, playedCount);
                if (cached?.AlbumArt is { Length: > 0 })
                    itemVM.AlbumCover = LoadBitmapImageFromBytes(cached.AlbumArt);
                PlaylistItems.Add(itemVM);
            }

            if (PlaylistItems.Count > 0)
            {
                var autoPlay = IsMusicPlayingQuery?.Invoke() ?? false;
                logger.LogInformation("OpenPlaylistAsync: loaded {Count} items, playing first", PlaylistItems.Count);
                PlaySongRequested?.Invoke(PlaylistItems[0].FilePath, autoPlay);
            }

            logger.LogInformation("OpenPlaylistAsync: user triggered open clean playlist, mark dirty as false");
            _isPlaylistDirty = false;
        }
        catch (Exception ex)
        {
            logger.LogError(ex, "OpenPlaylistAsync: failed to load playlist from {Path}", path);
            WpfMessageBox.Show($"加载播放列表失败: {ex.Message}\n{path}", "Error", WpfMessageBoxIcon.Error);
        }
    }

    [RelayCommand]
    public async Task SavePlaylistAsync()
    {
        bool isPlaylistOpened = false;
        logger.LogInformation("SavePlaylistAsync: saving playlist");
        var path = playlistProvider.CurrentFilePath;

        if (string.IsNullOrEmpty(path))
        {
            if (_isFileDialogOpen) return;
            _isFileDialogOpen = true;
            path = await fileDialogService.SaveWpplAsync(PlaylistTitle);
            _isFileDialogOpen = false;
            if (path == null) return;
        }
        else
        {
            isPlaylistOpened = true;
        }

        try
        {
            var playlist = playlistProvider.GetPlaylist();
            if (string.IsNullOrEmpty(playlist.Id))
                playlist.Id = Guid.NewGuid().ToString();
            playlist.FormatVersion = 3;
            playlist.CreatedAt = DateTimeOffset.Now;
            playlist.Name = PlaylistTitle;
            if (PlaylistCoverImage is not null)
            {
                playlist.Cover.Type = CoverType.Base64;
                var encoder = new JpegBitmapEncoder
                {
                    QualityLevel = 75
                };

                encoder.Frames.Add(BitmapFrame.Create(PlaylistCoverImage));

                using var ms = new MemoryStream();
                encoder.Save(ms);
                playlist.Cover.Data = ms.ToArray();
                playlist.Contents.Clear();
            }

            foreach (var item in PlaylistItems)
            {
                var md5 = ComputeFileMd5(item.FilePath);
                playlist.Contents.Add(new ContentRecord { File = item.FilePath, Md5 = md5 });
            }

            var errorCode = playlistProvider.Save(path);
            if (errorCode != IPlaylistProvider.ErrorCode.NoError)
            {
                logger.LogWarning("SavePlaylistAsync: save failed with error: {ErrorCode}", errorCode);
                WpfMessageBox.Show($"保存播放列表失败: {errorCode}", "Error", WpfMessageBoxIcon.Error);
            }
            else
            {
                logger.LogInformation("SavePlaylistAsync: playlist saved to {Path}, {Count} items", path, PlaylistItems.Count);
                _isPlaylistDirty = false;
                if (isPlaylistOpened)
                {
                    WpfMessageBox.Show($"已保存到{path}", "保存成功", WpfMessageBoxIcon.Information);
                }
            }
        }
        catch (Exception ex)
        {
            logger.LogError(ex, "SavePlaylistAsync: failed to save playlist to {Path}", path);
            WpfMessageBox.Show($"保存播放列表失败: {ex.Message}\n{path}", "Error", WpfMessageBoxIcon.Error);
        }
    }

    [RelayCommand]
    private async Task AddSongToPlaylistAsync()
    {
        if (_isFileDialogOpen) return;
        _isFileDialogOpen = true;
        logger.LogInformation("AddSongToPlaylistAsync: opening file dialog for adding songs");
        var paths = await fileDialogService.PickMusicFilesAsync();
        _isFileDialogOpen = false;

        foreach (var path in paths)
        {
            if (!File.Exists(path)) continue;
            if (PlaylistItems.Any(p => p.FilePath == path)) continue;

            var md5 = ComputeFileMd5(path);
            var cached = songDatabase.FindByMd5(md5);
            var title = cached?.Title ?? Path.GetFileNameWithoutExtension(path);
            var artist = cached?.Artist ?? "Unknown Artist";
            var playedCount = cached?.PlayCount ?? 0;

            var itemVM = new PlaylistItemViewModel(path, title, artist, playedCount);
            if (cached?.AlbumArt is { Length: > 0 })
                itemVM.AlbumCover = LoadBitmapImageFromBytes(cached.AlbumArt);

            PlaylistItems.Add(itemVM);
            logger.LogInformation("AddSongToPlaylistAsync: added {Title} to playlist", title);
            _isPlaylistUserOpened = true;
            _isPlaylistDirty = true;
        }
    }

    [RelayCommand]
    private void RemoveSongFromPlaylist(PlaylistItemViewModel? item)
    {
        if (item == null) return;
        logger.LogInformation("RemoveSongFromPlaylist: removing {Title} from playlist", item.Title);
        PlaylistItems.Remove(item);
        _isPlaylistUserOpened = true;
        _isPlaylistDirty = true;
        RemovePlaylistItemRequested?.Invoke(item.FilePath);
    }

    [RelayCommand]
    private async Task ChangePlaylistCoverAsync()
    {
        if (_isFileDialogOpen) return;
        _isFileDialogOpen = true;
        var path = await fileDialogService.PickImageAsync();
        _isFileDialogOpen = false;

        if (string.IsNullOrEmpty(path) || !File.Exists(path)) return;

        logger.LogInformation("ChangePlaylistCoverAsync: loading cover from {Path}", path);
        try
        {
            var imageBytes = await File.ReadAllBytesAsync(path);
            var compressedBytes = CompressImageToJpeg(imageBytes, 300, 300);

            var playlist = playlistProvider.GetPlaylist();
            playlist.Cover = new CoverRecord
            {
                Type = CoverType.Base64,
                Data = compressedBytes,
                Url = null
            };

            PlaylistCoverImage = LoadBitmapImageFromBytes(compressedBytes);
            _isPlaylistUserOpened = true;
            _isPlaylistDirty = true;
        }
        catch (Exception ex)
        {
            logger.LogError(ex, "ChangePlaylistCoverAsync: failed to set cover from {Path}", path);
            WpfMessageBox.Show($"设置封面失败: {ex.Message}", "Error", WpfMessageBoxIcon.Error);
        }
    }

    public void ClearPlaylist()
    {
        PlaylistItems.Clear();
        PlaylistTitle = "播放列表";
        PlaylistCoverImage = null;

        if (_isPlaylistUserOpened)
            _isPlaylistDirty = true;
        ResetPlaylistRequested?.Invoke();
    }

    [RelayCommand]
    public async Task CreatePlaylistAsync()
    {
        if (HasUnsavedPlaylistChanges)
        {
            var result = WpfMessageBox.Show(
                "播放列表有未保存的更改，是否保存？",
                "确认",
                WpfMessageBoxButton.YesNoCancel,
                WpfMessageBoxIcon.Question);

            switch (result)
            {
                case WpfMessageBoxResult.Yes:
                    await SavePlaylistAsync();
                    break;
                case WpfMessageBoxResult.Cancel:
                    return;
            }
        }

        playlistProvider.CreateDefault();
        ClearPlaylist();
        _isPlaylistUserOpened = true;
        _isPlaylistDirty = false;
    }

    [RelayCommand]
    public void SetAlbumArtFromPlaylistItem(PlaylistItemViewModel? item)
    {
        if (item?.AlbumCover is not null)
        {
            var albumCover = item.AlbumCover;
            PlaylistCoverImage = albumCover;
            _isPlaylistUserOpened = true;
            _isPlaylistDirty = true;
        }
    }
    public void PlayFromPlaylist(PlaylistItemViewModel item)
    {
        logger.LogInformation("PlayFromPlaylist: user selected {Title} ({FilePath})", item.Title, item.FilePath);
        PlaySongRequested?.Invoke(item.FilePath, true);
    }

    public void AddSong(string filePath, string? md5, string title, string artist, BitmapImage? fallbackAlbumCover)
    {
        if (PlaylistItems.Any(p => p.FilePath == filePath)) return;

        logger.LogInformation("AddSong: adding {FilePath} to playlist", filePath);
        var cached = md5 is not null ? songDatabase.FindByMd5(md5) : null;
        var itemVM = new PlaylistItemViewModel(filePath, title, artist, cached?.PlayCount ?? 0);
        if (cached?.AlbumArt is { Length: > 0 })
            itemVM.AlbumCover = LoadBitmapImageFromBytes(cached.AlbumArt);
        else if (fallbackAlbumCover is not null)
            itemVM.AlbumCover = fallbackAlbumCover;
        PlaylistItems.Add(itemVM);
        if (_isPlaylistUserOpened)
            _isPlaylistDirty = true;
    }

    public void SetPlayingItem(string? filePath)
    {
        foreach (var item in PlaylistItems)
            item.IsPlaying = item.FilePath == filePath;
    }

    public void UpdateItemMetadata(string? filePath, string? title, string? artist, BitmapImage? cover)
    {
        var item = PlaylistItems.FirstOrDefault(p => p.FilePath == filePath);
        if (cover is not null)
            item?.AlbumCover = cover;
        if (title is not null) 
            item?.Title = title;
        if (artist is not null)
            item?.Artist = artist;
    }

    public void IncrementItemPlayedCount(string? filePath)
    {
        var item = PlaylistItems.FirstOrDefault(p => p.FilePath == filePath);
        if (item is not null)
            item.PlayedCount++;
    }

    public int GetIndexByPath(string? filePath)
    {
        if (filePath == null) return -1;
        for (var i = 0; i < PlaylistItems.Count; i++)
        {
            if (PlaylistItems[i].FilePath == filePath)
                return i;
        }
        return -1;
    }

    private static BitmapImage LoadBitmapImageFromBytes(byte[] data)
    {
        using var memoryStream = new MemoryStream(data);
        var bitmapImage = new BitmapImage();
        bitmapImage.BeginInit();
        bitmapImage.CacheOption = BitmapCacheOption.OnLoad;
        bitmapImage.StreamSource = memoryStream;
        bitmapImage.EndInit();
        bitmapImage.Freeze();
        return bitmapImage;
    }

    private static string ComputeFileMd5(string filePath)
    {
        using var stream = File.OpenRead(filePath);
        var hashBytes = MD5.HashData(stream);
        return Convert.ToHexStringLower(hashBytes);
    }

    private static BitmapImage? TryLoadPlaylistCover(CoverRecord cover)
    {
        try
        {
            switch (cover.Type)
            {
                case CoverType.Base64 when cover.Data is { Length: > 0 }:
                    return LoadBitmapImageFromBytes(cover.Data);
                case CoverType.Local when !string.IsNullOrEmpty(cover.Url):
                    return !File.Exists(cover.Url) ? null : LoadBitmapImageFromBytes(File.ReadAllBytes(cover.Url));
                case CoverType.Cloud when !string.IsNullOrEmpty(cover.Url):
                    var bitmapImage = new BitmapImage();
                    bitmapImage.BeginInit();
                    bitmapImage.CacheOption = BitmapCacheOption.OnLoad;
                    bitmapImage.UriSource = new Uri(cover.Url, UriKind.Absolute);
                    bitmapImage.EndInit();
                    bitmapImage.Freeze();
                    return bitmapImage;
                default:
                    return null;
            }
        }
        catch { return null; }
    }

    private static byte[] CompressImageToJpeg(byte[] imageData, int maxWidth, int maxHeight)
    {
        using var inputStream = new MemoryStream(imageData);
        var decoder = BitmapDecoder.Create(inputStream, BitmapCreateOptions.PreservePixelFormat, BitmapCacheOption.OnLoad);
        var frame = decoder.Frames[0];

        var scaleX = (double)maxWidth / frame.PixelWidth;
        var scaleY = (double)maxHeight / frame.PixelHeight;
        var scale = Math.Min(scaleX, scaleY);
        if (scale > 1) scale = 1;

        var transformed = new TransformedBitmap(frame, new System.Windows.Media.ScaleTransform(scale, scale));
        var encoder = new JpegBitmapEncoder { QualityLevel = 80 };
        encoder.Frames.Add(BitmapFrame.Create(transformed));

        using var outputStream = new MemoryStream();
        encoder.Save(outputStream);
        return outputStream.ToArray();
    }
}