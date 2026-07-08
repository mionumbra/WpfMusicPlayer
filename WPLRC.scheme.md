# WPLRC Scheme

## Preamble

WPLRC is the intermediate lyric format used by WpfMusicPlayer. It is a UTF-8 encoded JSON format designed to preserve the semantic order of lyric text, translations, romanization lines, and optional word-level timing controller nodes after traditional LRC input has been normalized.

It is designed to be a successor format of traditional LRC, which has no clear schemes, no standards, and cannot preserve the semantic information between lyric nodes.

The current format version is `2`.

## Status

This document describes WPLRC format version 2 as produced by `LrcFileControllerNative::to_intermediate_json` and consumed by the managed lyric pipeline in WpfMusicPlayer.

Version 2 moves romanization schema information to the document root. Legacy version 1 documents may contain per-line `schema` or `scheme` properties; WpfMusicPlayer upgrades those documents to version 2 by inferring the dominant root-level `romanization_schema` and removing the per-line schema fields.

## File Format

A WPLRC file:

- MUST be valid standard JSON. JSON comments are not allowed.
- SHOULD be encoded as UTF-8. WpfMusicPlayer exports UTF-8 without BOM.
- SHOULD use the `.wplrc` file extension.
- MUST contain a top-level JSON object.

All time values are integers measured in milliseconds.

## Top-Level Object

| Property | Type | Required for v2 producers | Description |
| --- | --- | --- | --- |
| `format_version` | integer | Yes | Format version. Current value is `2`. |
| `romanization_schema` | string | No | Global romanization notation schema. Valid values are `romaji` and `jyutping`. Omit when no romanization schema is active. |
| `offset` | integer | Yes | Global lyric offset in milliseconds. Positive values delay lyrics; negative values advance lyrics. Consumers SHOULD default missing offsets to `0` for compatibility. |
| `metadata` | object | Yes | Metadata converted from LRC tags. May be an empty object. |
| `lyric_lines` | array | Yes | Ordered array of lyric line nodes. |

Consumers SHOULD reject documents whose `format_version` is unsupported. WpfMusicPlayer currently accepts version `1` for upgrade and version `2` as the current format.

## Metadata Object

The `metadata` object contains optional string fields. Producers SHOULD omit empty metadata fields.

| Property | Source LRC tags | Description |
| --- | --- | --- |
| `artist` | `ar`, `artist` | Artist name. |
| `album` | `al`, `album` | Album name. |
| `author` | `au`, `author` | Lyric author. |
| `by` | `by` | LRC file creator/uploader field. |
| `title` | `ti`, `title` | Song title. |

The LRC `[offset:...]` tag is represented by the top-level `offset` property, not inside `metadata`.

## Lyric Line Object

Each item in `lyric_lines` represents one timed lyric group.

| Property | Type | Required | Description |
| --- | --- | --- | --- |
| `time_start_ms` | integer | Yes | Start time of this lyric group. |
| `time_end_ms` | integer | Yes | End time of this lyric group. |
| `lines` | array | Yes | Semantic line nodes that belong to this timed group. |

`lyric_lines` SHOULD be sorted by ascending `time_start_ms`. A producer SHOULD ensure `time_end_ms > time_start_ms`; and if a consumer is procedding with a node with `time_start_ms` > `time_end_ms`, it SHOULD fail immediately. If an LRC line has no explicit end time, WpfMusicPlayer derives it from the next line start time, from word-level controller timing, from song duration when available, or from a minimal non-zero fallback duration.

## Line Node Object

Each item in a lyric line's `lines` array represents one semantic text line.

| Property | Type | Required                                                                              | Description |
| --- | --- | --- | --- |
| `role` | string | Yes                                                                                   | Semantic role. Valid values are `lyric`, `translation`, `romanization`, and `ignored`. |
| `language` | string | Recommended                                                                           | Detected language classifier output. See valid values below. |
| `text` | string | Required when `sync` is absent; when `sync` is present, it will be discarded into silence. | Plain text for this semantic line. |
| `sync` | string | No                                                                                    | Synchronization type. Use `controller_nodes` for word-level timing. |
| `controller_nodes` | array | Required when `sync` is `controller_nodes`                                            | Word-level or segment-level timing controller nodes. |

Valid `language` values currently produced by WpfMusicPlayer are:
- `zh` -- Simplified Chinese / Traditional Chinese
- `latin` -- English / France / Italian / Spanish, and / or other latin-based languages
- `jp` -- Japanese, with Hiragana, Katakana, and Kanji
- `kr` -- Korean, with Hangul letters and Hanja
- `ru` -- Russian / Mongolia , and / or other cyrillic alphabet-based languages
- `jyut` -- Cantonese Jyutping and Hanyu Pinyin
- `roma` -- Romanization of Japanese and Korean
- `onomatopoeia` -- Onomatopoeia with no actual meaning, or other unrecogized languages

Consumers SHOULD treat unknown `role`, `language`, or `sync` values conservatively. WpfMusicPlayer reads roles case-insensitively and uses the first `lyric` role as the primary display line. If no `lyric` role exists, it falls back to the first line node in the group.

## Controller Nodes

When a line node has `"sync": "controller_nodes"`, the line uses timed controller nodes instead of plain text timing.

| Property | Type | Required | Description |
| --- | --- | --- | --- |
| `time_start_ms` | integer | Yes | Start time of this controller segment. |
| `time_end_ms` | integer | Yes | End time of this controller segment. |
| `text` | string | Yes | Text displayed during this controller segment. |

Controller node times are absolute document times in milliseconds, not offsets relative to their parent line.

A controller-synchronized line SHOULD omit `text`; consumers can reconstruct the display text by concatenating `controller_nodes[*].text` in order, discarding the `text` property into silence. WpfMusicPlayer does this reconstruction when `text` is absent.

For highlight progress, WpfMusicPlayer interpolates linearly inside each controller node and maps the active node index plus local progress to the whole line's progress. Empty controller node text is ignored by the managed display pipeline.

For legacy compatibility, WpfMusicPlayer also recognizes `sync: "controller_node"` as equivalent to `sync: "controller_nodes"`, but version 2 producers SHOULD write only `controller_nodes`.

## Romanization Schema

`romanization_schema` is a document-level property in version 2. It describes the notation system used by lines whose role is `romanization`.

Valid values:

- `romaji`: romanized Japanese or Korean-style romanization as classified by the WpfMusicPlayer lyric engine.
- `jyutping`: Cantonese Jyutping and / or Hanyu Pinyin.

If `romanization_schema` is missing or unknown, consumers SHOULD treat romanization as present only when `romanization` role lines exist, but SHOULD NOT assume a specific notation system.

Version 2 line nodes MUST NOT use `schema` or `scheme`. Consumers MAY remove those fields during normalization.

## Example

This is a complete WPLRC version 2 document. The example is valid JSON and contains no comments.

```json
{
  "format_version": 2,
  "romanization_schema": "romaji",
  "offset": 0,
  "metadata": {
    "artist": "sample",
    "album": "sample",
    "author": "sample",
    "by": "sample",
    "title": "sample"
  },
  "lyric_lines": [
    {
      "time_start_ms": 12060,
      "time_end_ms": 13843,
      "lines": [
        {
          "role": "romanization",
          "sync": "controller_nodes",
          "language": "roma",
          "controller_nodes": [
            {
              "time_start_ms": 12060,
              "time_end_ms": 12215,
              "text": "ba "
            },
            {
              "time_start_ms": 12216,
              "time_end_ms": 12371,
              "text": "'d "
            },
            {
              "time_start_ms": 12371,
              "time_end_ms": 12507,
              "text": "do "
            },
            {
              "time_start_ms": 12508,
              "time_end_ms": 12655,
              "text": "ra "
            },
            {
              "time_start_ms": 12656,
              "time_end_ms": 12803,
              "text": "n "
            },
            {
              "time_start_ms": 12803,
              "time_end_ms": 12955,
              "text": "do "
            },
            {
              "time_start_ms": 12956,
              "time_end_ms": 13123,
              "text": "ni "
            },
            {
              "time_start_ms": 13123,
              "time_end_ms": 13275,
              "text": "u "
            },
            {
              "time_start_ms": 13276,
              "time_end_ms": 13428,
              "text": "ma "
            },
            {
              "time_start_ms": 13428,
              "time_end_ms": 13556,
              "text": "re "
            },
            {
              "time_start_ms": 13557,
              "time_end_ms": 13843,
              "text": "ta "
            }
          ]
        },
        {
          "role": "lyric",
          "language": "jp",
          "text": "バッドランドに生まれた"
        },
        {
          "role": "translation",
          "language": "zh",
          "text": "只因诞生于劣地"
        }
      ]
    },
    {
      "time_start_ms": 13843,
      "time_end_ms": 16267,
      "lines": [
        {
          "role": "romanization",
          "sync": "controller_nodes",
          "language": "roma",
          "controller_nodes": [
            {
              "time_start_ms": 13843,
              "time_end_ms": 13987,
              "text": "da "
            },
            {
              "time_start_ms": 13987,
              "time_end_ms": 14154,
              "text": "ke "
            },
            {
              "time_start_ms": 14154,
              "time_end_ms": 14313,
              "text": "de "
            },
            {
              "time_start_ms": 14314,
              "time_end_ms": 14474,
              "text": "ba "
            },
            {
              "time_start_ms": 14474,
              "time_end_ms": 14634,
              "text": "'d "
            },
            {
              "time_start_ms": 14635,
              "time_end_ms": 14786,
              "text": "do "
            },
            {
              "time_start_ms": 14786,
              "time_end_ms": 14938,
              "text": "ra "
            },
            {
              "time_start_ms": 14939,
              "time_end_ms": 15116,
              "text": "i "
            },
            {
              "time_start_ms": 15116,
              "time_end_ms": 15258,
              "text": "fu "
            },
            {
              "time_start_ms": 15258,
              "time_end_ms": 15417,
              "text": "ga "
            },
            {
              "time_start_ms": 15418,
              "time_end_ms": 15554,
              "text": "de "
            },
            {
              "time_start_ms": 15555,
              "time_end_ms": 15630,
              "text": "fo "
            },
            {
              "time_start_ms": 15706,
              "time_end_ms": 15842,
              "text": "to "
            },
            {
              "time_start_ms": 15843,
              "time_end_ms": 16266,
              "text": "ka "
            }
          ]
        },
        {
          "role": "lyric",
          "language": "jp",
          "text": "だけでバッドライフがデフォとか"
        },
        {
          "role": "translation",
          "language": "zh",
          "text": "就默认会拥有糟糕的人生吗"
        }
      ]
    }
  ]
}
```

## Minimal Example

```json
{
  "format_version": 2,
  "offset": 0,
  "metadata": {},
  "lyric_lines": [
    {
      "time_start_ms": 1000,
      "time_end_ms": 5500,
      "lines": [
        {
          "role": "lyric",
          "language": "latin",
          "text": "First line"
        }
      ]
    }
  ]
}
```

## Version Compatibility

WPLRC version 1 is a legacy intermediate format. Its known difference is that romanization schema may appear on individual line nodes as `schema` or `scheme`.

When upgrading version 1 to version 2, and current WPLRC version 1 has romanization line nodes, WpfMusicPlayer:

1. Counts recognized per-line schema values.
2. Chooses `jyutping` if it appears more often than `romaji`; otherwise chooses `romaji` when any recognized romanization schema exists.
3. Writes the chosen value to root-level `romanization_schema`.
4. Sets `format_version` to `2`.
5. Removes all per-line `scheme`(and `schemea`, if contains) properties.

Otherwise, it does not produce `romanization_schema` root property.

Version 2 producers SHOULD NOT emit version 1 fields.

## Producer Notes

The WpfMusicPlayer LRC converter currently normalizes several LRC patterns into WPLRC:

- Plain line-timed LRC.
- Multiple time tags on one LRC line.
- Out-of-order LRC timestamps, sorted stably by time.
- Interleaved or synchronized translation lines.
- Inline translations in supported patterns.
- Extended LRC or word-level timing nodes using `<mm:ss.xxx>` or `[mm:ss.xxx]` style controller markers.

Those source-LRC heuristics are producer behavior, not additional WPLRC syntax. A WPLRC reader only needs to implement the JSON format described above.

