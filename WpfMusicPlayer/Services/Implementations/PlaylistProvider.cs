// SPDX-License-Identifier: MIT

using System.IO;
using System.Text.Json;
using WpfMusicPlayer.Models;
using WpfMusicPlayer.Services.Abstractions;
using static WpfMusicPlayer.Services.Abstractions.IPlaylistProvider;

namespace WpfMusicPlayer.Services.Implementations;

public class PlaylistProvider : IPlaylistProvider
{
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        WriteIndented = true
    };

    private PlaylistRecord _playlist = new();

    public string? CurrentFilePath { get; private set; }

    public ErrorCode Load(string filePath)
    {
        try
        {
            if (!File.Exists(filePath))
                return ErrorCode.FileNotFound;

            var json = File.ReadAllText(filePath);
            var record = JsonSerializer.Deserialize<PlaylistRecord>(json, JsonOptions);
            if (record is null)
                return ErrorCode.ParseError;

            _playlist = record;
            CurrentFilePath = filePath;
            return ErrorCode.NoError;
        }
        catch (UnauthorizedAccessException)
        {
            return ErrorCode.PermissionDenied;
        }
        catch (JsonException)
        {
            return ErrorCode.ParseError;
        }
        catch (IOException)
        {
            return ErrorCode.FileOpenFailed;
        }
        catch
        {
            return ErrorCode.UnknownError;
        }
    }

    public ErrorCode Save(string filePath)
    {
        try
        {
            var json = JsonSerializer.Serialize(_playlist, JsonOptions);
            File.WriteAllText(filePath, json);
            CurrentFilePath = filePath;
            return ErrorCode.NoError;
        }
        catch (UnauthorizedAccessException)
        {
            return ErrorCode.PermissionDenied;
        }
        catch (IOException)
        {
            return ErrorCode.FileOpenFailed;
        }
        catch
        {
            return ErrorCode.UnknownError;
        }
    }

    public ErrorCode CreateDefault()
    {
        _playlist = new PlaylistRecord
        {
            FormatVersion = 1,
            Id = Guid.NewGuid().ToString(),
            Name = "播放列表",
            CreatedAt = DateTimeOffset.Now,
            PlaybackSettings = new PlaybackSettingsRecord
            {
                SortMode = SortMode.Manual,
                LoopMode = LoopMode.None,
                IsDecreasing = false
            },
            Cover = new CoverRecord { Type = CoverType.Local }
        };
        CurrentFilePath = string.Empty;
        return ErrorCode.NoError;
    }

    public ref PlaylistRecord GetPlaylist()
    {
        return ref _playlist;
    }
}