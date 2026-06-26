// SPDX-License-Identifier: MIT

using System.Text.Json.Serialization;

namespace WpfMusicPlayer.Models;

[JsonConverter(typeof(JsonStringEnumConverter<SortMode>))]
public enum SortMode
{
    [JsonStringEnumMemberName("manual")]
    Manual,

    [JsonStringEnumMemberName("artist")]
    Artist,

    [JsonStringEnumMemberName("title")]
    Title
}

[JsonConverter(typeof(JsonStringEnumConverter<LoopMode>))]
public enum LoopMode
{
    [JsonStringEnumMemberName("none")]
    None,

    [JsonStringEnumMemberName("seq")]
    Seq,

    [JsonStringEnumMemberName("loop")]
    Loop,

    [JsonStringEnumMemberName("single_loop")]
    SingleLoop,

    [JsonStringEnumMemberName("shuffle")]
    Shuffle
}

[JsonConverter(typeof(JsonStringEnumConverter<CoverType>))]
public enum CoverType
{
    [JsonStringEnumMemberName("local")]
    Local,

    [JsonStringEnumMemberName("base64")]
    Base64,

    [JsonStringEnumMemberName("cloud")]
    Cloud
}

public class PlaylistRecord
{
    [JsonPropertyName("format_version")]
    public int FormatVersion { get; set; }

    [JsonPropertyName("id")]
    public string Id { get; set; } = string.Empty;

    [JsonPropertyName("name")]
    public string Name { get; set; } = string.Empty;

    [JsonPropertyName("description")]
    public string Description { get; set; } = string.Empty;

    [JsonPropertyName("author")]
    public string Author { get; set; } = string.Empty;

    [JsonPropertyName("created_at")]
    public DateTimeOffset CreatedAt { get; set; }

    [JsonPropertyName("tags")]
    public List<string> Tags { get; set; } = [];

    [JsonPropertyName("playback_settings")]
    public PlaybackSettingsRecord PlaybackSettings { get; set; } = new();

    [JsonPropertyName("cover")]
    public CoverRecord Cover { get; set; } = new();

    [JsonPropertyName("contents")]
    public List<ContentRecord> Contents { get; set; } = [];
}

public class PlaybackSettingsRecord
{
    [JsonPropertyName("sort_mode")]
    public SortMode SortMode { get; set; } = SortMode.Manual;

    [JsonPropertyName("is_decreasing")]
    public bool IsDecreasing { get; set; }

    [JsonPropertyName("loop_mode")]
    public LoopMode LoopMode { get; set; } = LoopMode.None;
}

public class CoverRecord
{
    [JsonPropertyName("type")]
    public CoverType Type { get; set; } = CoverType.Local;

    [JsonPropertyName("url")]
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public string? Url { get; set; }

    [JsonPropertyName("data")]
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public byte[]? Data { get; set; }

    // Base64有且仅有Type==Base64时启用，URL同理
    [JsonIgnore]
    public bool IsValid => Type switch
    {
        CoverType.Base64 => Data is { Length: > 0 } && Url is null,
        _                => Url is not null && Data is null
    };
}

public class ContentRecord
{
    [JsonPropertyName("file")]
    public string File { get; set; } = string.Empty;

    [JsonPropertyName("md5")]
    public string Md5 { get; set; } = string.Empty;
}