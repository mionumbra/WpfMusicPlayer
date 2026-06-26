// SPDX-License-Identifier: MIT

using Microsoft.Extensions.Configuration;
using WpfMusicPlayer.Services.Abstractions;
using WpfMusicPlayer.ViewModels;

namespace WpfMusicPlayer.Services.Implementations;

public class CommandLineParser : ICommandLineParser
{
    private readonly IConfiguration _configuration;

    public CommandLineParser(string[] args)
    {
        var switchMappings = new Dictionary<string, string>
        {
            { "--file", "FilePath" },
            { "-f", "FilePath" },
            { "--time", "MusicCurrentTime" },
            { "-t", "MusicCurrentTime" },
            { "--autostart", "AutoStart" },
            { "-a", "AutoStart" },
            { "--view", "StartupView" },
            { "--playlist", "OpenedPlaylistPath" },
            { "-p", "OpenedPlaylistPath" },
            { "--playmode", "CurrentPlayMode" },
            { "--desktoplyric", "IsDesktopLyricToggled" },
            { "--translation", "TranslationToggled" },
            { "--romanji", "RomanjiToggled" }
        };

        _configuration = new ConfigurationBuilder()
            .AddCommandLine(args, switchMappings)
            .Build();
    }

    public string FilePath =>
        _configuration["FilePath"] ?? string.Empty;

    public float MusicCurrentTime =>
        float.TryParse(_configuration["MusicCurrentTime"], out var time) ? time : 0f;

    public bool AutoStart =>
        bool.TryParse(_configuration["AutoStart"], out var auto) && auto;

    public ActiveView StartupView =>
        Enum.TryParse<ActiveView>(_configuration["StartupView"], true, out var view)
            ? view
            : ActiveView.Player;

    public string OpenedPlaylistPath =>
        _configuration["OpenedPlaylistPath"] ?? string.Empty;

    public bool TranslationToggled =>
        bool.TryParse(_configuration["TranslationToggled"], out var t) && t;

    public bool RomanjiToggled =>
        bool.TryParse(_configuration["RomanjiToggled"], out var r) && r;
    
    public PlayMode CurrentPlayMode =>
        Enum.TryParse<PlayMode>(_configuration["CurrentPlayMode"], true, out var mode)
            ? mode : PlayMode.Sequential;
    
    public bool IsDesktopLyricToggled =>
        bool.TryParse(_configuration["IsDesktopLyricToggled"], out var d) && d;

    public int[] AppliedEqualizerSettings
    {
        get
        {
            var section = _configuration.GetSection("eq");
            var children = section.GetChildren().ToList();
            if (children.Count == 0)
                return [];
            return children
                .OrderBy(c => int.TryParse(c.Key, out var k) ? k : 0)
                .Select(c => int.TryParse(c.Value, out var v) ? v : 0)
                .ToArray();
        }
    }
}