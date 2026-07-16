// SPDX-License-Identifier: MIT

using Microsoft.Extensions.Logging;

namespace WpfMusicPlayer.Helpers;

public class NativeLoggerBridge
{
    private readonly ILogger _logger;

    public NativeLoggerBridge(ILogger<NativeLoggerBridge> logger)
    {
        _logger = logger;
    }

    // for native method to invoke using reflection
    // ReSharper disable once UnusedMember.Global
    public void LogTrace(string message) => _logger.LogTrace("{Message}", message);
    
    // ReSharper disable once UnusedMember.Global
    public void LogDebug(string message) => _logger.LogDebug("{Message}", message);
    
    // ReSharper disable once UnusedMember.Global
    public void LogInformation(string message) => _logger.LogInformation("{Message}", message);
    
    // ReSharper disable once UnusedMember.Global
    public void LogWarning(string message) => _logger.LogWarning("{Message}", message);
    
    // ReSharper disable once UnusedMember.Global
    public void LogError(string message) => _logger.LogError("{Message}", message);
}
