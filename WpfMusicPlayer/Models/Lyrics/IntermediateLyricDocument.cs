// SPDX-License-Identifier: MIT
using System.Text.Json;
using System.Text.Json.Serialization;

namespace WpfMusicPlayer.Models.Lyrics;

public enum RomanizationScheme
{
    Romaji,
    Jyutping,
    ErrorOrNotEnabled
}

public sealed class IntermediateLyricDocument
{
    public static readonly JsonSerializerOptions JsonOptions = new()
    {
        PropertyNameCaseInsensitive = true
    };

    [JsonPropertyName("format_version")]
    public int FormatVersion { get; init; }

    [JsonPropertyName("offset")]
    public int Offset { get; init; }

    [JsonPropertyName("metadata")]
    public IntermediateLyricMetadata Metadata { get; init; } = new();

    [JsonPropertyName("lyric_lines")]
    public List<IntermediateLyricLine> LyricLines { get; init; } = [];

    public static IntermediateLyricDocument FromJson(string json)
    {
        var document = JsonSerializer.Deserialize<IntermediateLyricDocument>(json, JsonOptions)
            ?? throw new InvalidOperationException("Invalid intermediate lyric data.");

        if (document.FormatVersion <= 0)
            throw new InvalidOperationException("Invalid intermediate lyric format version.");

        return document;
    }

    public static bool HasIntermediateSchema(string json)
    {
        try
        {
            using var document = JsonDocument.Parse(json);
            var root = document.RootElement;

            return root.ValueKind == JsonValueKind.Object
                   && root.TryGetProperty("format_version", out var version)
                   && version.ValueKind == JsonValueKind.Number
                   && root.TryGetProperty("lyric_lines", out var lyricLines)
                   && lyricLines.ValueKind == JsonValueKind.Array;
        }
        catch (JsonException)
        {
            return false;
        }
    }
}

public sealed class IntermediateLyricMetadata
{
    [JsonPropertyName("artist")]
    public string? Artist { get; init; }

    [JsonPropertyName("album")]
    public string? Album { get; init; }

    [JsonPropertyName("author")]
    public string? Author { get; init; }

    [JsonPropertyName("by")]
    public string? By { get; init; }

    [JsonPropertyName("title")]
    public string? Title { get; init; }
}

public sealed class IntermediateLyricLine
{
    [JsonPropertyName("time_start_ms")]
    public int TimeStartMs { get; init; }

    [JsonPropertyName("time_end_ms")]
    public int TimeEndMs { get; init; }

    [JsonPropertyName("lines")]
    public List<IntermediateLineNode> Lines { get; init; } = [];

    [JsonIgnore]
    public int ControllerLineIndex => Lines
        .Select((line, index) => new { line, index })
        .Where(x => x.line.HasControllerNodesSync)
        .Aggregate(
            new { index = -1, count = -1 },
            (best, cur) => cur.line.ControllerNodes.Count > best.count
                ? new { index = cur.index, count = cur.line.ControllerNodes.Count }
                : best)
        .index;
}

public sealed class IntermediateLineNode
{
    [JsonPropertyName("role")]
    public string? Role { get; init; }

    [JsonPropertyName("text")]
    public string? Text { get; init; }

    [JsonPropertyName("sync")]
    public string? Sync { get; init; }

    [JsonPropertyName("language")]
    public string? Language { get; init; }

    [JsonPropertyName("scheme")]
    public string? Scheme { get; init; }

    [JsonPropertyName("controller_nodes")]
    public List<IntermediateControllerNode> ControllerNodes { get; init; } = [];

    [JsonIgnore]
    public bool HasControllerNodesSync =>
        string.Equals(Sync, "controller_node", StringComparison.OrdinalIgnoreCase)
        || string.Equals(Sync, "controller_nodes", StringComparison.OrdinalIgnoreCase);

    [JsonIgnore]
    public RomanizationScheme RomanizationScheme =>
        Scheme switch
        {
            "romaji" => RomanizationScheme.Romaji,
            "jyutping" => RomanizationScheme.Jyutping,
            _ => RomanizationScheme.ErrorOrNotEnabled
        };
}

public sealed class IntermediateControllerNode
{
    [JsonPropertyName("time_start_ms")]
    public int TimeStartMs { get; init; }

    [JsonPropertyName("time_end_ms")]
    public int TimeEndMs { get; init; }

    [JsonPropertyName("text")]
    public string? Text { get; init; }
}
