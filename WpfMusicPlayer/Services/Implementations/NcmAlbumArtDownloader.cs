// SPDX-License-Identifier: MIT

using System.Buffers;
using System.IO;
using System.Net;
using System.Net.Http;
using Microsoft.Extensions.Logging;
using WpfMusicPlayer.Services.Abstractions;

namespace WpfMusicPlayer.Services.Implementations;

public sealed class NcmAlbumArtDownloader(ILogger<NcmAlbumArtDownloader> logger) : INcmAlbumArtDownloader
{
    private const int MaxDownloadBytes = 10 * 1024 * 1024;
    private const int BufferSize = 32 * 1024;
    private static readonly TimeSpan ConnectTimeout = TimeSpan.FromSeconds(5);
    private static readonly TimeSpan ReceiveTimeout = TimeSpan.FromSeconds(5);

    private static readonly HashSet<string> AllowedHosts = new(StringComparer.OrdinalIgnoreCase)
    {
        "p1.music.126.net",
        "p2.music.126.net",
        "p3.music.126.net",
        "p4.music.126.net"
    };

    public async Task<byte[]?> DownloadAsync(string url, CancellationToken cancellationToken = default)
    {
        if (string.IsNullOrWhiteSpace(url))
        {
            logger.LogDebug("NCM album art URL is empty.");
            return null;
        }

        var uri = ValidateUri(url);
        using var handler = new SocketsHttpHandler();
        handler.AllowAutoRedirect = false;
        handler.ConnectTimeout = ConnectTimeout;
        handler.AutomaticDecompression = DecompressionMethods.None;
        using var client = new HttpClient(handler);
        client.Timeout = Timeout.InfiniteTimeSpan;
        using var request = new HttpRequestMessage(HttpMethod.Get, uri);

        using var response = await SendAsync(client, request, cancellationToken).ConfigureAwait(false);
        if (IsRedirect(response.StatusCode))
            throw new InvalidOperationException("NCM album art redirects are not allowed.");

        response.EnsureSuccessStatusCode();
        var contentLength = response.Content.Headers.ContentLength;
        ValidateContentLength(contentLength);
        logger.LogInformation($"NCM album art content length: {contentLength}");

        await using var responseStream = await response.Content.ReadAsStreamAsync(cancellationToken).ConfigureAwait(false);
        return await ReadLimitedAsync(responseStream, cancellationToken).ConfigureAwait(false);
    }

    private static Uri ValidateUri(string url)
    {
        if (!Uri.TryCreate(url, UriKind.Absolute, out var uri))
            throw new ArgumentException("NCM album art URL is invalid.", nameof(url));

        if (uri.Scheme != Uri.UriSchemeHttps)
            throw new InvalidOperationException("NCM album art URL must use HTTPS.");

        var host = uri.IdnHost.TrimEnd('.');
        if (!AllowedHosts.Contains(host))
            throw new InvalidOperationException($"NCM album art host is not allowed: {host}");

        return uri;
    }

    private static async Task<HttpResponseMessage> SendAsync(
        HttpClient client,
        HttpRequestMessage request,
        CancellationToken cancellationToken)
    {
        using var receiveTimeout = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        receiveTimeout.CancelAfter(ReceiveTimeout);
        return await client
            .SendAsync(request, HttpCompletionOption.ResponseHeadersRead, receiveTimeout.Token)
            .ConfigureAwait(false);
    }

    private static void ValidateContentLength(long? contentLength)
    {
        if (contentLength > MaxDownloadBytes)
            throw new InvalidDataException("NCM album art exceeds the 10MB download limit.");
    }

    private static async Task<byte[]> ReadLimitedAsync(Stream stream, CancellationToken cancellationToken)
    {
        var buffer = ArrayPool<byte>.Shared.Rent(BufferSize);
        try
        {
            using var output = new MemoryStream();
            while (true)
            {
                var bytesRead = await ReadWithTimeoutAsync(stream, buffer, cancellationToken).ConfigureAwait(false);
                if (bytesRead == 0)
                    return output.ToArray();

                if (output.Length + bytesRead > MaxDownloadBytes)
                    throw new InvalidDataException("NCM album art exceeds the 10MB download limit.");

                output.Write(buffer, 0, bytesRead);
            }
        }
        finally
        {
            ArrayPool<byte>.Shared.Return(buffer);
        }
    }

    private static async Task<int> ReadWithTimeoutAsync(
        Stream stream,
        byte[] buffer,
        CancellationToken cancellationToken)
    {
        using var receiveTimeout = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        receiveTimeout.CancelAfter(ReceiveTimeout);
        return await stream.ReadAsync(buffer.AsMemory(0, buffer.Length), receiveTimeout.Token).ConfigureAwait(false);
    }

    private static bool IsRedirect(HttpStatusCode statusCode)
    {
        var code = (int)statusCode;
        return code is >= 300 and <= 399;
    }
}
