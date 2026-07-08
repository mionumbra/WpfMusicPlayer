// SPDX-License-Identifier: MIT

using System.IO;
using System.Text.Encodings.Web;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.Json.Serialization;

namespace WpfMusicPlayer.Models.Lyrics;

public enum RomanizationSchema
{
    Romaji,
    Jyutping,
    ErrorOrNotEnabled
}

public sealed record IntermediateLyricNormalizationResult(string Json, bool WasUpgraded);

public sealed class IntermediateLyricDocument
{
    public const int CurrentFormatVersion = 2;

    public static readonly JsonSerializerOptions JsonOptions = new()
    {
        PropertyNameCaseInsensitive = true,
        Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
        DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull
    };

    [JsonPropertyName("format_version")]
    public int FormatVersion { get; init; }

    [JsonPropertyName("romanization_schema")]
    public string? RomanizationSchemaString { get; init; }

    [JsonPropertyName("offset")]
    public int Offset { get; init; }

    [JsonPropertyName("metadata")]
    public IntermediateLyricMetadata Metadata { get; init; } = new();

    [JsonPropertyName("lyric_lines")]
    public List<IntermediateLyricLine> LyricLines { get; init; } = [];

    [JsonIgnore]
    public RomanizationSchema RomanizationSchema => ParseRomanizationSchema(RomanizationSchemaString);

    public static IntermediateLyricDocument FromJson(string json)
    {
        var normalized = NormalizeToCurrentVersion(json);
        var document = JsonSerializer.Deserialize<IntermediateLyricDocument>(normalized.Json, JsonOptions)
            ?? throw new InvalidOperationException("Invalid intermediate lyric data.");

        return document.FormatVersion != CurrentFormatVersion 
            ? throw new InvalidOperationException("Invalid intermediate lyric format version.")
            : document;
    }

    public static IntermediateLyricNormalizationResult NormalizeToCurrentVersion(string json)
    {
        JsonObject root;
        try
        {
            root = JsonNode.Parse(json) as JsonObject
                ?? throw new InvalidOperationException("Invalid intermediate lyric data.");
        }
        catch (JsonException ex)
        {
            throw new InvalidOperationException("Invalid intermediate lyric data.", ex);
        }

        var formatVersion = ReadFormatVersion(root);
        return formatVersion switch
        {
            1 => UpgradeV1ToCurrent(root),
            CurrentFormatVersion => NormalizeV2(root),
            _ => throw new InvalidOperationException($"Unsupported intermediate lyric format version: {formatVersion}.")
        };
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

    private static IntermediateLyricNormalizationResult UpgradeV1ToCurrent(JsonObject root)
    {

        var schema = InferLegacyRomanizationSchema(root);
        RemoveLegacyLineSchemas(root);

        root["format_version"] = CurrentFormatVersion;
        
        if (!string.IsNullOrEmpty(schema))
            root.Insert(root.IndexOf("format_version") + 1,
                "romanization_schema", schema);
        ValidateLyricLineTimeRanges(root);
        return new IntermediateLyricNormalizationResult(root.ToJsonString(JsonOptions), WasUpgraded: true);
    }

    private static IntermediateLyricNormalizationResult NormalizeV2(JsonObject root)
    {
        RejectLegacyLineSchemas(root);
        ValidateLyricLineTimeRanges(root);
        return new IntermediateLyricNormalizationResult(root.ToJsonString(JsonOptions), WasUpgraded: false);
    }

    private static int ReadFormatVersion(JsonObject root)
    {
        if (!root.TryGetPropertyValue("format_version", out var versionNode) || versionNode is null)
            throw new InvalidOperationException("Invalid intermediate lyric format version.");

        try
        {
            return versionNode.GetValue<int>();
        }
        catch (InvalidOperationException ex)
        {
            throw new InvalidOperationException("Invalid intermediate lyric format version.", ex);
        }
    }

    private static string InferLegacyRomanizationSchema(JsonObject root)
    {
        var romajiCount = 0;
        var jyutpingCount = 0;

        foreach (var lineNode in EnumerateLineNodes(root))
        {
            switch (ReadNormalizedSchema(lineNode))
            {
                case "jyutping":
                    jyutpingCount++;
                    break;
                case "romaji":
                    romajiCount++;
                    break;
            }
        }

        if (romajiCount == 0 && jyutpingCount == 0)
            return string.Empty;
        return jyutpingCount > romajiCount ? "jyutping" : "romaji";
    }

    private static IEnumerable<JsonObject> EnumerateLineNodes(JsonObject root)
    {
        if (root["lyric_lines"] is not JsonArray lyricLines)
            yield break;

        foreach (var lyricLineNode in lyricLines)
        {
            if (lyricLineNode is not JsonObject lyricLine
                || lyricLine["lines"] is not JsonArray lines)
            {
                continue;
            }

            foreach (var lineNode in lines)
            {
                if (lineNode is JsonObject line)
                    yield return line;
            }
        }
    }

    private static void RemoveLegacyLineSchemas(JsonObject root)
    {
        foreach (var lineNode in EnumerateLineNodes(root))
        {
            lineNode.Remove("schema");
            lineNode.Remove("scheme");
        }
    }

    private static void RejectLegacyLineSchemas(JsonObject root)
    {
        if (EnumerateLineNodes(root)
            .Any(lineNode => 
                lineNode.ContainsKey("schema") 
                || lineNode.ContainsKey("scheme")))
        {
            throw new InvalidOperationException("Version 2 intermediate lyric lines must not contain schema or scheme.");
        }
    }

    private static void ValidateLyricLineTimeRanges(JsonObject root)
    {
        if (root["lyric_lines"] is not JsonArray lyricLines)
            return;

        foreach (var lyricLineNode in lyricLines)
        {
            if (lyricLineNode is not JsonObject lyricLine)
                continue;

            if (TryReadIntProperty(lyricLine, "time_start_ms", out var startMs)
                && TryReadIntProperty(lyricLine, "time_end_ms", out var endMs)
                && startMs > endMs)
            {
                throw new InvalidOperationException("Invalid intermediate lyric line time range.");
            }
        }
    }

    private static bool TryReadIntProperty(JsonObject node, string name, out int value)
    {
        value = 0;
        if (!node.TryGetPropertyValue(name, out var valueNode) || valueNode is null)
            return false;

        try
        {
            value = valueNode.GetValue<int>();
            return true;
        }
        catch (InvalidOperationException)
        {
            return false;
        }
        catch (FormatException)
        {
            return false;
        }
    }

    private static string? ReadNormalizedSchema(JsonObject node)
    {
        return NormalizeSchema(
            ReadStringProperty(node, "schema")
            ?? ReadStringProperty(node, "scheme"));
    }

    private static string? ReadStringProperty(JsonObject node, string name)
    {
        if (!node.TryGetPropertyValue(name, out var valueNode) || valueNode is null)
            return null;

        try
        {
            return valueNode.GetValue<string>();
        }
        catch (InvalidOperationException)
        {
            return null;
        }
    }

    private static string? NormalizeSchema(string? schema)
    {
        return schema?.Trim().ToLowerInvariant() switch
        {
            "romaji" => "romaji",
            "jyutping" => "jyutping",
            _ => null
        };
    }

    private static RomanizationSchema ParseRomanizationSchema(string? schema)
    {
        return NormalizeSchema(schema) switch
        {
            "romaji" => RomanizationSchema.Romaji,
            "jyutping" => RomanizationSchema.Jyutping,
            _ => RomanizationSchema.ErrorOrNotEnabled
        };
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

    [JsonPropertyName("controller_nodes")]
    public List<IntermediateControllerNode> ControllerNodes { get; init; } = [];

    [JsonIgnore]
    public bool HasControllerNodesSync =>
        string.Equals(Sync, "controller_node", StringComparison.OrdinalIgnoreCase)
        || string.Equals(Sync, "controller_nodes", StringComparison.OrdinalIgnoreCase);
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
