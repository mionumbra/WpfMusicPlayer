// SPDX-License-Identifier: MIT

namespace WpfMusicPlayer.Services.Abstractions;

public interface INcmAlbumArtDownloader
{
    Task<byte[]?> DownloadAsync(string url, CancellationToken cancellationToken = default);
}
