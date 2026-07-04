// SPDX-License-Identifier: MIT

using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using System.Windows;
using Microsoft.Extensions.Configuration;
using WpfMusicPlayer.Helpers;
using WpfMusicPlayer.Services.Abstractions;
using WpfMusicPlayer.Services.Implementations;
using WpfMusicPlayer.ViewModels;
using WpfMusicPlayer.Views;
using Serilog;

namespace WpfMusicPlayer;

/// <summary>
/// Interaction logic for App.xaml
/// </summary>
public partial class App : Application
{
    private readonly IHost _host;

    public App()
    {
        _host =
            Host.CreateDefaultBuilder()
                .ConfigureAppConfiguration((context, config) =>
                {
                    config.AddJsonFile("WpfMusicPlayer.appsettings.json", optional: false, reloadOnChange: true);
                    config.AddJsonFile("WpfMusicPlayer.appsettings.Local.json", optional: true, reloadOnChange: true);
                })
                .UseSerilog((hostContext, services, loggerConfig) =>
                {
                    loggerConfig
                        .ReadFrom.Configuration(hostContext.Configuration) 
                        .ReadFrom.Services(services);
                })
                .ConfigureServices((_, services) =>
                {
                    services.AddSingleton<NativeLoggerBridge>();

                    services.AddSingleton<IConfigProvider, ConfigProvider>();
                    services.AddSingleton<ISmtcService, SmtcService>();
                    services.AddSingleton<ISongDatabaseService, SongDatabaseService>();
                    services.AddSingleton<IPlaylistProvider, PlaylistProvider>();
                    services.AddSingleton<ILyricParser, IntermediateLyricParser>();
                    services.AddSingleton<ILyricParser, LrcFileParser>();
                    services.AddSingleton<LyricParserFactory>();

                    services.AddTransient<INcmAlbumArtDownloader, NcmAlbumArtDownloader>();
                    services.AddTransient<IFileDialogService, FileDialogService>();
                    services.AddTransient<ICommandLineParser>(_ =>
                        new CommandLineParser(Environment.GetCommandLineArgs()));
                    services.AddTransient<Func<INcmAlbumArtDownloader>>(serviceProvider =>
                        () => serviceProvider.GetRequiredService<INcmAlbumArtDownloader>());

                    services.AddSingleton<PlaylistViewModel>();
                    services.AddSingleton<LyricsViewModel>();
                    services.AddSingleton<DesktopLyricViewModel>();
                    services.AddSingleton<DesktopTrayIconViewModel>();
                    services.AddSingleton<MainViewModel>();

                    services.AddSingleton<DesktopTrayIcon>();
                    services.AddSingleton<MainWindow>();
                })
                .Build();
    }

    protected override async void OnStartup(StartupEventArgs e)
    {
        await _host.StartAsync();

        var loggerBridge = _host.Services.GetRequiredService<NativeLoggerBridge>();
        MusicPlayerLibrary.NativeTraceRedirectManager.Init(loggerBridge);

        var mainWindow = _host.Services.GetRequiredService<MainWindow>();
        mainWindow.Show();

        _host.Services.GetRequiredService<DesktopTrayIcon>();

        base.OnStartup(e);
    }

    protected override async void OnExit(ExitEventArgs e)
    {
        using (_host)
        {
            await _host.StopAsync();
        }

        base.OnExit(e);
    }
}
