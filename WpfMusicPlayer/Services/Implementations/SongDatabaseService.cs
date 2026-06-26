// SPDX-License-Identifier: MIT

using System.IO;
using LiteDB;
using Microsoft.Extensions.Logging;
using WpfMusicPlayer.Models;
using WpfMusicPlayer.Services.Abstractions;

namespace WpfMusicPlayer.Services.Implementations;

public class SongDatabaseService : ISongDatabaseService
{
    private readonly LiteDatabase _db;
    private readonly ILiteCollection<SongRecord> _songs;
    private readonly ILogger<SongDatabaseService> _logger;

    public SongDatabaseService(ILogger<SongDatabaseService> logger, string databasePath = "Songs.db")
    {
        _logger = logger;
        for (var i = 0; i < 5; ++i)
        {
            try
            {
                _logger.LogInformation("Loading songs from database {DatabasePath}", databasePath);
                _db = new LiteDatabase(databasePath);
                _songs = _db.GetCollection<SongRecord>("songs");
                _songs.EnsureIndex(s => s.Md5, unique: true);
                return;
            }
            catch (IOException e)
            {
                Thread.Sleep(500);
            }
        }
        throw new IOException("Database not found, or occupied by another program");
    }

    public SongRecord? FindByMd5(string md5)
    {
        return _songs.FindOne(s => s.Md5 == md5);
    }

    public void Upsert(SongRecord record)
    {
        var existing = FindByMd5(record.Md5);
        if (existing is not null)
        {
            record.Id = existing.Id;
            record.PlayCount = existing.PlayCount;
            _songs.Update(record);
        }
        else
        {
            _songs.Insert(record);
        }
    }

    public void IncrementPlayCount(string md5)
    {
        var record = FindByMd5(md5);
        if (record is not null)
        {
            record.PlayCount++;
            _songs.Update(record);
        }
    }

    public List<SongRecord> GetAllSongs()
    {
        return [.. _songs.FindAll()];
    }

    public List<SongRecord> GetTopPlayed(int count)
    {
        return _songs.Query()
            .OrderByDescending(s => s.PlayCount)
            .Limit(count)
            .ToList();
    }

    public void Dispose() => _db.Dispose();
}
