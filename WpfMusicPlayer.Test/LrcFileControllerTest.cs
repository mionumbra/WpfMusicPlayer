// SPDX-License-Identifier: MIT

using System.Text;
using System.Text.Json;
using Microsoft.Extensions.Logging.Abstractions;
using WpfMusicPlayer.Models.Lyrics;
using WpfMusicPlayer.Services.Abstractions;
using WpfMusicPlayer.Services.Implementations;
using WpfMusicPlayer.ViewModels;

namespace WpfMusicPlayer.Test;

[TestClass]
public sealed class LrcFileControllerTest
{
    private const string SimpleLrc =
        """
        [ar:TestArtist]
        [ti:TestTitle]
        [al:TestAlbum]
        [by:TestBy]
        [offset:0]
        [00:01.00]First line
        [00:05.50]Second line
        [00:10.00]Third line
        """;

    private static JsonDocument ParseToJson(string lrc, int songEndTimeMs = 0)
    {
        var parser = new LrcFileParser();
        var json = parser.ParseToIntermediateJson(new LyricParserSource(lrc, songEndTimeMs: songEndTimeMs));
        Assert.IsFalse(string.IsNullOrWhiteSpace(json));
        return JsonDocument.Parse(json);
    }

    private static LyricParserFactory CreateLyricParserFactory()
    {
        ILyricParser[] parsers = [new IntermediateLyricParser(), new LrcFileParser()];
        return new LyricParserFactory(parsers);
    }

    private static JsonElement[] GetLyricLines(JsonDocument doc)
    {
        return doc.RootElement.GetProperty("lyric_lines").EnumerateArray().ToArray();
    }

    private static JsonElement[] GetLineNodes(JsonElement lyricLine)
    {
        return lyricLine.GetProperty("lines").EnumerateArray().ToArray();
    }

    private static JsonElement GetRoleLine(JsonElement lyricLine, string role)
    {
        return GetLineNodes(lyricLine).Single(line => line.GetProperty("role").GetString() == role);
    }

    private static void AssertNoLineNodeSchema(JsonDocument doc)
    {
        foreach (var line in GetLyricLines(doc))
        foreach (var lineNode in GetLineNodes(line))
        {
            Assert.IsFalse(lineNode.TryGetProperty("scheme", out _));
            Assert.IsFalse(lineNode.TryGetProperty("schema", out _));
        }
    }

    private static LyricsViewModel CreateLyricsViewModel()
    {
        return new LyricsViewModel(
            NullLogger<LyricsViewModel>.Instance,
            new NoopFileDialogService(),
            CreateLyricParserFactory());
    }

    [TestMethod]
    public void ParseLrcToIntermediateJson_SimpleLrc_ExportsSortedLyricLines()
    {
        using var doc = ParseToJson(SimpleLrc);

        var root = doc.RootElement;
        Assert.AreEqual(2, root.GetProperty("format_version").GetInt32());
        Assert.AreEqual(0, root.GetProperty("offset").GetInt32());
        Assert.AreEqual("TestArtist", root.GetProperty("metadata").GetProperty("artist").GetString());
        Assert.AreEqual("TestTitle", root.GetProperty("metadata").GetProperty("title").GetString());
        Assert.AreEqual("TestAlbum", root.GetProperty("metadata").GetProperty("album").GetString());
        Assert.AreEqual("TestBy", root.GetProperty("metadata").GetProperty("by").GetString());

        var lines = GetLyricLines(doc);
        Assert.HasCount(3, lines);
        Assert.AreEqual(1000, lines[0].GetProperty("time_start_ms").GetInt32());
        Assert.AreEqual(5500, lines[0].GetProperty("time_end_ms").GetInt32());
        Assert.AreEqual("First line", GetRoleLine(lines[0], "lyric").GetProperty("text").GetString());
        Assert.AreEqual(5500, lines[1].GetProperty("time_start_ms").GetInt32());
        Assert.AreEqual("Second line", GetRoleLine(lines[1], "lyric").GetProperty("text").GetString());
        Assert.AreEqual(10000, lines[2].GetProperty("time_start_ms").GetInt32());
        Assert.AreEqual("Third line", GetRoleLine(lines[2], "lyric").GetProperty("text").GetString());
        AssertNoLineNodeSchema(doc);
    }

    [TestMethod]
    public void ParseLrcToIntermediateJson_LastPlainLine_ExportsNonZeroDuration()
    {
        const string lrc = "[00:10.00]Last line";

        using var doc = ParseToJson(lrc);
        var line = GetLyricLines(doc).Single();
        var startMs = line.GetProperty("time_start_ms").GetInt32();
        var endMs = line.GetProperty("time_end_ms").GetInt32();

        Assert.AreEqual(10000, startMs);
        Assert.IsGreaterThan(startMs, endMs, $"Expected last line end time to be greater than start time, got {startMs}..{endMs}.");
    }

    [TestMethod]
    public void ParseLrcToIntermediateJson_LastPlainLine_UsesSongEndTime()
    {
        const string lrc = "[00:10.00]Last line";

        using var doc = ParseToJson(lrc, 15000);
        var line = GetLyricLines(doc).Single();

        Assert.AreEqual(10000, line.GetProperty("time_start_ms").GetInt32());
        Assert.AreEqual(15000, line.GetProperty("time_end_ms").GetInt32());
    }

    [TestMethod]
    public void ParseLrcToIntermediateJson_LastProgressLine_DoesNotUseSongEndTime()
    {
        const string lrc = "[00:00.00]A<00:00.250>B<00:00.500>";

        using var doc = ParseToJson(lrc, 10000);
        var line = GetLyricLines(doc).Single();
        var controllerNodes = GetLineNodes(line).Single().GetProperty("controller_nodes").EnumerateArray().ToArray();

        Assert.AreEqual(0, line.GetProperty("time_start_ms").GetInt32());
        Assert.AreEqual(500, line.GetProperty("time_end_ms").GetInt32());
        Assert.HasCount(2, controllerNodes);
    }

    [TestMethod]
    public void ParseLrcToIntermediateJson_TimeFormats_NormalizesMilliseconds()
    {
        const string lrc =
            """
            [00:02.05]Line A
            [00:03.123]Line B
            [00:04.1000]Line C
            """;

        using var doc = ParseToJson(lrc);
        var lines = GetLyricLines(doc);

        Assert.HasCount(3, lines);
        Assert.AreEqual(2050, lines[0].GetProperty("time_start_ms").GetInt32());
        Assert.AreEqual(3123, lines[1].GetProperty("time_start_ms").GetInt32());
        Assert.AreEqual(4100, lines[2].GetProperty("time_start_ms").GetInt32());
    }

    [TestMethod]
    public void ParseLrcToIntermediateJson_DisorderedAndMultipleTimeTags_ExportsSortedCopies()
    {
        const string lrc = "[00:05.00]B\n[00:01.00]A\n[00:03.00][00:02.00]C";

        using var doc = ParseToJson(lrc);
        var lines = GetLyricLines(doc);

        Assert.HasCount(4, lines);
        Assert.AreEqual(1000, lines[0].GetProperty("time_start_ms").GetInt32());
        Assert.AreEqual("A", GetRoleLine(lines[0], "lyric").GetProperty("text").GetString());
        Assert.AreEqual(2000, lines[1].GetProperty("time_start_ms").GetInt32());
        Assert.AreEqual("C", GetRoleLine(lines[1], "lyric").GetProperty("text").GetString());
        Assert.AreEqual(3000, lines[2].GetProperty("time_start_ms").GetInt32());
        Assert.AreEqual("C", GetRoleLine(lines[2], "lyric").GetProperty("text").GetString());
        Assert.AreEqual(5000, lines[3].GetProperty("time_start_ms").GetInt32());
    }

    [TestMethod]
    public void ParseLrcToIntermediateJson_ProgressNode_ExportsControllerNodes()
    {
        const string lrc = """
                           [offset:100]
                           [00:00.00]今<00:00.250>天<00:00.500>我<00:00.750>
                           """;

        using var doc = ParseToJson(lrc);

        var root = doc.RootElement;
        Assert.AreEqual(100, root.GetProperty("offset").GetInt32());

        var lyricLine = GetLyricLines(doc).Single();
        Assert.AreEqual(0, lyricLine.GetProperty("time_start_ms").GetInt32());
        Assert.AreEqual(750, lyricLine.GetProperty("time_end_ms").GetInt32());

        var line = GetLineNodes(lyricLine).Single();
        Assert.AreEqual("lyric", line.GetProperty("role").GetString());
        Assert.AreEqual("controller_nodes", line.GetProperty("sync").GetString());

        var controllerNodes = line.GetProperty("controller_nodes").EnumerateArray().ToArray();
        Assert.HasCount(3, controllerNodes);
        Assert.AreEqual(0, controllerNodes[0].GetProperty("time_start_ms").GetInt32());
        Assert.AreEqual(250, controllerNodes[0].GetProperty("time_end_ms").GetInt32());
        Assert.AreEqual("今", controllerNodes[0].GetProperty("text").GetString());
        Assert.AreEqual(250, controllerNodes[1].GetProperty("time_start_ms").GetInt32());
        Assert.AreEqual(500, controllerNodes[1].GetProperty("time_end_ms").GetInt32());
        Assert.AreEqual("天", controllerNodes[1].GetProperty("text").GetString());
        Assert.AreEqual(500, controllerNodes[2].GetProperty("time_start_ms").GetInt32());
        Assert.AreEqual(750, controllerNodes[2].GetProperty("time_end_ms").GetInt32());
        Assert.AreEqual("我", controllerNodes[2].GetProperty("text").GetString());
    }

    [TestMethod]
    public void ParseLrcToIntermediateJson_Metadata_ExportsPresentFieldsOnly()
    {
        const string lrc = """
                           [ar:TestArtist]
                           [ti:TestTitle]
                           [by:TestBy]
                           [00:01.00]Line
                           """;

        using var doc = ParseToJson(lrc);

        var metadata = doc.RootElement.GetProperty("metadata");
        Assert.AreEqual("TestArtist", metadata.GetProperty("artist").GetString());
        Assert.AreEqual("TestTitle", metadata.GetProperty("title").GetString());
        Assert.AreEqual("TestBy", metadata.GetProperty("by").GetString());
        Assert.IsFalse(metadata.TryGetProperty("album", out _));
        Assert.IsFalse(metadata.TryGetProperty("author", out _));
    }

    [TestMethod]
    public void LyricParserFactory_WplrcFile_ReturnsIntermediateAstWithoutLrcParsing()
    {
        var path = Path.Combine(Path.GetTempPath(), $"{Guid.NewGuid():N}.wplrc");
        const string json = """
                            {
                              "format_version": 1,
                              "offset": 250,
                              "metadata": {
                                "title": "Internal"
                              },
                              "lyric_lines": [
                                {
                                  "time_start_ms": 1000,
                                  "time_end_ms": 2000,
                                  "lines": [
                                    {
                                      "role": "lyric",
                                      "text": "Internal line"
                                    }
                                  ]
                                }
                              ]
                            }
                            """;

        try
        {
            var ast = IntermediateLyricDocument.FromJson(json);

            Assert.AreEqual(2, ast.FormatVersion);
            Assert.AreEqual(RomanizationSchema.ErrorOrNotEnabled, ast.RomanizationSchema);
            Assert.AreEqual(250, ast.Offset);
            Assert.AreEqual("Internal", ast.Metadata.Title);
            Assert.HasCount(1, ast.LyricLines);
            Assert.AreEqual("Internal line", ast.LyricLines[0].Lines[0].Text);
        }
        finally
        {
            if (File.Exists(path))
                File.Delete(path);
        }
    }

    [TestMethod]
    public void LyricParserFactory_IntermediateJsonContent_ReturnsIntermediateAst()
    {
        const string json = """
                            {
                              "format_version": 1,
                              "offset": 0,
                              "lyric_lines": [
                                {
                                  "time_start_ms": 3000,
                                  "time_end_ms": 4000,
                                  "lines": [
                                    {
                                      "role": "lyric",
                                      "text": "Schema matched"
                                    }
                                  ]
                                }
                              ]
                            }
                            """;

        var ast = IntermediateLyricDocument.FromJson(json);

        Assert.AreEqual(2, ast.FormatVersion);
        Assert.AreEqual(RomanizationSchema.ErrorOrNotEnabled, ast.RomanizationSchema);
        Assert.HasCount(1, ast.LyricLines);
        Assert.AreEqual(3000, ast.LyricLines[0].TimeStartMs);
        Assert.AreEqual("Schema matched", ast.LyricLines[0].Lines[0].Text);
    }

    [TestMethod]
    public void IntermediateLyricParser_V1PromotesMostCommonSchemaAndRemovesLineSchemas()
    {
        const string json = """
                            {
                              "format_version": 1,
                              "offset": 0,
                              "lyric_lines": [
                                {
                                  "time_start_ms": 0,
                                  "time_end_ms": 1000,
                                  "lines": [
                                    {
                                      "role": "lyric",
                                      "text": "粤语"
                                    },
                                    {
                                      "role": "romanization",
                                      "scheme": "jyutping",
                                      "text": "jyut one"
                                    }
                                  ]
                                },
                                {
                                  "time_start_ms": 1000,
                                  "time_end_ms": 2000,
                                  "lines": [
                                    {
                                      "role": "lyric",
                                      "text": "你的母亲"
                                    },
                                    {
                                      "role": "romanization",
                                      "scheme": "jyutping",
                                      "text": "jyut two"
                                    }
                                  ]
                                }
                              ]
                            }
                            """;

        var result = new IntermediateLyricParser().Parse(new LyricParserSource(json));

        Assert.IsTrue(result.ShouldWriteBack);
        using var doc = JsonDocument.Parse(result.IntermediateJson);
        Assert.AreEqual(2, doc.RootElement.GetProperty("format_version").GetInt32());
        Assert.AreEqual("jyutping", doc.RootElement.GetProperty("romanization_schema").GetString());
        AssertNoLineNodeSchema(doc);
    }

    [TestMethod]
    public void IntermediateLyricParser_WplrcFileRejectsNonUtf8Bytes()
    {
        var path = Path.Combine(Path.GetTempPath(), $"{Guid.NewGuid():N}.wplrc");
        var prefix = Encoding.ASCII.GetBytes("""
                                             {"format_version":2,"offset":0,"metadata":{},"lyric_lines":[
                                             """);
        var suffix = Encoding.ASCII.GetBytes("]}");
        var invalidBytes = new byte[prefix.Length + 1 + suffix.Length];
        Buffer.BlockCopy(prefix, 0, invalidBytes, 0, prefix.Length);
        invalidBytes[prefix.Length] = 0xFF;
        Buffer.BlockCopy(suffix, 0, invalidBytes, prefix.Length + 1, suffix.Length);

        try
        {
            File.WriteAllBytes(path, invalidBytes);

            Assert.ThrowsExactly<InvalidOperationException>(() =>
                new IntermediateLyricParser().Parse(new LyricParserSource(path)));
        }
        finally
        {
            if (File.Exists(path))
                File.Delete(path);
        }
    }

    [TestMethod]
    public void IntermediateLyricParser_V2RejectsLegacyLineSchema()
    {
        const string json = """
                            {
                              "format_version": 2,
                              "offset": 0,
                              "lyric_lines": [
                                {
                                  "time_start_ms": 0,
                                  "time_end_ms": 1000,
                                  "lines": [
                                    {
                                      "role": "romanization",
                                      "scheme": "romaji",
                                      "text": "legacy"
                                    }
                                  ]
                                }
                              ]
                            }
                            """;

        Assert.ThrowsExactly<InvalidOperationException>(() =>
            new IntermediateLyricParser().Parse(new LyricParserSource(json)));
    }

    [TestMethod]
    public void IntermediateLyricParser_RejectsLineWithStartAfterEnd()
    {
        const string json = """
                            {
                              "format_version": 2,
                              "offset": 0,
                              "lyric_lines": [
                                {
                                  "time_start_ms": 2000,
                                  "time_end_ms": 1000,
                                  "lines": [
                                    {
                                      "role": "lyric",
                                      "text": "bad range"
                                    }
                                  ]
                                }
                              ]
                            }
                            """;

        Assert.ThrowsExactly<InvalidOperationException>(() => IntermediateLyricDocument.FromJson(json));
    }

    [TestMethod]
    public void IntermediateLyricParser_AllowsLineWithEqualStartAndEnd()
    {
        const string json = """
                            {
                              "format_version": 2,
                              "offset": 0,
                              "lyric_lines": [
                                {
                                  "time_start_ms": 1000,
                                  "time_end_ms": 1000,
                                  "lines": [
                                    {
                                      "role": "lyric",
                                      "text": "legacy equal range"
                                    }
                                  ]
                                }
                              ]
                            }
                            """;

        var document = IntermediateLyricDocument.FromJson(json);

        Assert.HasCount(1, document.LyricLines);
        Assert.AreEqual(1000, document.LyricLines[0].TimeStartMs);
        Assert.AreEqual(1000, document.LyricLines[0].TimeEndMs);
    }

    [TestMethod]
    public void LyricParserFactory_SupportedOpenExtensions_ReturnsRegisteredParserExtensions()
    {
        var extensions = CreateLyricParserFactory().SupportedOpenExtensions.ToArray();

        CollectionAssert.AreEqual(new[] { ".wplrc", ".lrc" }, extensions);
    }

    [TestMethod]
    public void ParseLrcToIntermediateJson_PartitionedTranslationBlock_ExportsTranslationRole()
    {
        const string lrc = """
                           [00:27.12]마음이 울적하고 답답할 땐
                           [00:30.87]산으로 올라가 소릴 한번 질러봐
                           [00:34.29]나처럼 이렇게 가슴을 펴고
                           
                           [00:27.12]当心中忧郁寂寞又烦闷之时
                           [00:30.87]上山去喊出来吧
                           [00:34.29]像我这样打开心扉
                           """;

        using var doc = ParseToJson(lrc);
        var lines = GetLyricLines(doc);

        Assert.HasCount(3, lines);
        Assert.AreEqual(27120, lines[0].GetProperty("time_start_ms").GetInt32());
        Assert.AreEqual("当心中忧郁寂寞又烦闷之时", GetRoleLine(lines[0], "translation").GetProperty("text").GetString());
        Assert.AreEqual("上山去喊出来吧", GetRoleLine(lines[1], "translation").GetProperty("text").GetString());
        Assert.AreEqual("像我这样打开心扉", GetRoleLine(lines[2], "translation").GetProperty("text").GetString());
    }

    [TestMethod]
    public void ParseLrcToIntermediateJson_BracketInlineTranslation_SplitsBeforeClassification()
    {
        const string lrc = """
                           [00:01.00]Hello (live) （翻译： 你好（世界）（现场））
                           [00:02.00]World(翻译:世界)
                           """;

        using var doc = ParseToJson(lrc);
        var lines = GetLyricLines(doc);

        Assert.HasCount(2, lines);
        Assert.AreEqual("Hello (live)", GetRoleLine(lines[0], "lyric").GetProperty("text").GetString());
        Assert.AreEqual("你好（世界）（现场）", GetRoleLine(lines[0], "translation").GetProperty("text").GetString());
        Assert.AreEqual("World", GetRoleLine(lines[1], "lyric").GetProperty("text").GetString());
        Assert.AreEqual("世界", GetRoleLine(lines[1], "translation").GetProperty("text").GetString());
    }

    [TestMethod]
    public void ParseLrcToIntermediateJson_SlashInlineTranslation_WhenMajority_SplitsBeforeClassification()
    {
        const string lrc = """
                           [00:01.00]Hello / 你好
                           [00:02.00]World / 世界
                           [00:03.00]Good night
                           """;

        using var doc = ParseToJson(lrc);
        var lines = GetLyricLines(doc);

        Assert.HasCount(3, lines);
        Assert.AreEqual("Hello", GetRoleLine(lines[0], "lyric").GetProperty("text").GetString());
        Assert.AreEqual("你好", GetRoleLine(lines[0], "translation").GetProperty("text").GetString());
        Assert.AreEqual("World", GetRoleLine(lines[1], "lyric").GetProperty("text").GetString());
        Assert.AreEqual("世界", GetRoleLine(lines[1], "translation").GetProperty("text").GetString());
        Assert.AreEqual("Good night", GetRoleLine(lines[2], "lyric").GetProperty("text").GetString());
    }

    [TestMethod]
    public void ParseLrcToIntermediateJson_SlashInlineTranslation_WhenBelowThreshold_KeepsOriginalLine()
    {
        const string lrc = """
                           [00:01.00]AC / DC
                           [00:02.00]Plain line
                           [00:03.00]Another line
                           """;

        using var doc = ParseToJson(lrc);
        var lineNodes = GetLineNodes(GetLyricLines(doc)[0]);

        Assert.HasCount(1, lineNodes);
        Assert.AreEqual("lyric", lineNodes[0].GetProperty("role").GetString());
        Assert.AreEqual("AC / DC", lineNodes[0].GetProperty("text").GetString());
    }

    [TestMethod]
    public void ParseLrcToIntermediateJson_MalformedTimeTag_MetadataParsing()
    {
        const string lrc = """
                           [00:00.000]作词 : MIMI
                           [00:00.000][by:gurantouw]
                           [00:00.000][al:MIMI]
                           [00:00.211]作曲 : MIMI
                           [00:00.211][ar:MIMI]
                           """;

        using var doc = ParseToJson(lrc);
        var metadata = doc.RootElement.GetProperty("metadata");

        Assert.AreEqual("gurantouw", metadata.GetProperty("by").GetString());
        Assert.AreEqual("MIMI", metadata.GetProperty("album").GetString());
        Assert.AreEqual("MIMI", metadata.GetProperty("artist").GetString());
    }

    [TestMethod]
    public void ParseLrcToIntermediateJson_MalformedRomanjiContainsChinese_ExportsRoles()
    {
        const string lrc = """
                           [01:15.836]be tsu no ko to ga n ba re ba i i ja n ( 笑 )
                           [01:15.836]別の事頑張ればいいじゃん(笑)
                           [01:15.836]在其他方面全力以赴不就行了(笑)
                           """;

        using var doc = ParseToJson(lrc);
        var lineNodes = GetLineNodes(GetLyricLines(doc).Single());

        Assert.AreEqual("romanization", lineNodes[0].GetProperty("role").GetString());
        Assert.AreEqual("translation", lineNodes[2].GetProperty("role").GetString());
    }

    [TestMethod]
    public void ParseLrcToIntermediateJson_MalformedRomanjiContainsChineseComposer_ExportsRoles()
    {
        const string lrc = """
                           [00:03.697]kyo ku ： Aiobahn
                           [00:03.697]曲：Aiobahn
                           """;

        using var doc = ParseToJson(lrc);
        var lineNodes = GetLineNodes(GetLyricLines(doc).Single());

        Assert.AreEqual("romanization", lineNodes[0].GetProperty("role").GetString());
        Assert.AreEqual("lyric", lineNodes[1].GetProperty("role").GetString());
    }

    [TestMethod]
    public void ParseLrcToIntermediateJson_ChnWithJyutping_ExportsRomanizationRole()
    {
        const string lrc = """
                           [00:06.417]coi san dou coi san dou 
                           [00:06.417]财神到财神到
                           [00:08.080]hou san da hou bou 
                           [00:08.080]好心得好报
                           """;

        using var doc = ParseToJson(lrc);
        var lines = GetLyricLines(doc);

        Assert.AreEqual("jyutping", doc.RootElement.GetProperty("romanization_schema").GetString());
        Assert.AreEqual("romanization", GetLineNodes(lines[0])[0].GetProperty("role").GetString());
        Assert.AreEqual("romanization", GetLineNodes(lines[1])[0].GetProperty("role").GetString());
        AssertNoLineNodeSchema(doc);
    }

    [TestMethod]
    public void ParseLrcToIntermediateJson_MalformedTimeTag_Throws()
    {
        const string lrc = "[INVALID]Some text";

        Assert.ThrowsExactly<InvalidOperationException>(() => CreateLyricParserFactory().ParseToIntermediateJson(lrc));
    }

    [TestMethod]
    public void ParseLrcToIntermediateJson_UnclosedOpeningBracket_Throws()
    {
        const string lrc = "[00:01.000 lyric without closing bracket";

        Assert.ThrowsExactly<InvalidOperationException>(() => CreateLyricParserFactory().ParseToIntermediateJson(lrc));
    }

    [TestMethod]
    public void LyricsViewModel_LoadLyrics_UsesIntermediateJsonControllerNodes()
    {
        const string lrc = """
                           [00:00.00]今<00:00.250>天<00:00.500>我<00:00.750>
                           [00:02.00]End
                           """;

        var vm = CreateLyricsViewModel();
        vm.LoadLyricsAsync(null, lrc, null, 10f, 0).Wait();
        vm.UpdateLyricProgress(0.375f);

        Assert.HasCount(2, vm.Lyrics);
        Assert.AreEqual("今天我", vm.Lyrics[0].Text);
        Assert.IsTrue(vm.Lyrics[0].IsProgressEnabled);
        Assert.AreEqual(0, vm.CurrentLyricIndex);
        Assert.AreEqual(0.5, vm.Lyrics[0].Progress, 0.001);
    }

    [TestMethod]
    public void LyricsViewModel_LoadLyrics_RebuildsControllerTextWhenTextIsPresent()
    {
        const string json = """
                            {
                              "format_version": 2,
                              "offset": 0,
                              "lyric_lines": [
                                {
                                  "time_start_ms": 0,
                                  "time_end_ms": 1000,
                                  "lines": [
                                    {
                                      "role": "lyric",
                                      "sync": "controller_nodes",
                                      "text": "wrong text",
                                      "controller_nodes": [
                                        {
                                          "time_start_ms": 0,
                                          "time_end_ms": 500,
                                          "text": "A"
                                        },
                                        {
                                          "time_start_ms": 500,
                                          "time_end_ms": 1000,
                                          "text": "B"
                                        }
                                      ]
                                    }
                                  ]
                                }
                              ]
                            }
                            """;

        var vm = CreateLyricsViewModel();
        vm.LoadLyricsAsync(null, json, null, 10f, 0).Wait();

        Assert.HasCount(1, vm.Lyrics);
        Assert.AreEqual("AB", vm.Lyrics[0].Text);
        Assert.IsTrue(vm.Lyrics[0].IsProgressEnabled);
    }

    [TestMethod]
    public void LyricsViewModel_LoadLyrics_UsesLinearProgressWhenSyncIsAbsent()
    {
        const string lrc = """
                           [00:01.00]Plain
                           [00:02.00]Next
                           """;

        var vm = CreateLyricsViewModel();
        vm.LoadLyricsAsync(null, lrc, null, 10f, 0).Wait();
        vm.UpdateLyricProgress(1.5f);

        Assert.HasCount(2, vm.Lyrics);
        Assert.AreEqual("Plain", vm.Lyrics[0].Text);
        Assert.IsFalse(vm.Lyrics[0].IsProgressEnabled);
        Assert.AreEqual(0, vm.CurrentLyricIndex);
        Assert.AreEqual(0.5, vm.Lyrics[0].Progress, 0.001);
    }

    [TestMethod]
    public void LyricsViewModel_LoadLyrics_AppliesIntermediateOffsetUnlessOverridden()
    {
        const string lrc = """
                           [offset:500]
                           [00:01.00]A
                           [00:02.00]B
                           """;

        var metadataOffsetVm = CreateLyricsViewModel();
        metadataOffsetVm.LoadLyricsAsync(null, lrc, null, 10f, 0).Wait();

        var overrideOffsetVm = CreateLyricsViewModel();
        overrideOffsetVm.LoadLyricsAsync(null, lrc, null, 10f, 200).Wait();

        Assert.AreEqual(1500, metadataOffsetVm.Lyrics[0].TimeMs);
        Assert.AreEqual(1200, overrideOffsetVm.Lyrics[0].TimeMs);
    }

    [TestMethod]
    public void LyricsViewModel_LoadLyrics_PersistsDatabaseCachedLrcAsIntermediateJson()
    {
        const string lrc = "[00:01.00]Stored";
        var vm = CreateLyricsViewModel();
        string? storedLyric = null;
        int? storedOffset = null;
        vm.UpdateCurrentLyricRequested += (content, offsetMs) =>
        {
            storedLyric = content;
            storedOffset = offsetMs;
        };

        vm.LoadLyricsAsync(null, lrc, null, 10f, 0, SuppliedLyricSource.DatabaseCache).Wait();

        Assert.IsNotNull(storedLyric);
        Assert.AreEqual(0, storedOffset);
        using var doc = JsonDocument.Parse(storedLyric);
        Assert.AreEqual(2, doc.RootElement.GetProperty("format_version").GetInt32());
        var line = GetLyricLines(doc).Single();
        Assert.AreEqual("Stored", GetRoleLine(line, "lyric").GetProperty("text").GetString());
    }

    [TestMethod]
    public void LyricsViewModel_LoadLyrics_PersistsDatabaseCachedV1IntermediateJsonAsV2()
    {
        const string json = """
                            {
                              "format_version": 1,
                              "offset": 0,
                              "lyric_lines": [
                                {
                                  "time_start_ms": 1000,
                                  "time_end_ms": 2000,
                                  "lines": [
                                    {
                                      "role": "lyric",
                                      "text": "Cached JSON"
                                    }
                                  ]
                                }
                              ]
                            }
                            """;
        var vm = CreateLyricsViewModel();
        string? storedLyric = null;
        vm.UpdateCurrentLyricRequested += (content, _) => storedLyric = content;

        vm.LoadLyricsAsync(null, json, null, 10f, 0, SuppliedLyricSource.DatabaseCache).Wait();

        Assert.IsNotNull(storedLyric);
        using var doc = JsonDocument.Parse(storedLyric);
        Assert.AreEqual(2, doc.RootElement.GetProperty("format_version").GetInt32());
        AssertNoLineNodeSchema(doc);
        Assert.HasCount(1, vm.Lyrics);
        Assert.AreEqual("Cached JSON", vm.Lyrics[0].Text);
    }

    [TestMethod]
    public void LyricsViewModel_LoadLyrics_DoesNotPersistDatabaseCachedV2IntermediateJson()
    {
        const string json = """
                            {
                              "format_version": 2,
                              "romanization_schema": "romaji",
                              "offset": 0,
                              "lyric_lines": [
                                {
                                  "time_start_ms": 1000,
                                  "time_end_ms": 2000,
                                  "lines": [
                                    {
                                      "role": "lyric",
                                      "text": "Cached JSON"
                                    }
                                  ]
                                }
                              ]
                            }
                            """;
        var vm = CreateLyricsViewModel();
        var writeBackRequested = false;
        vm.UpdateCurrentLyricRequested += (_, _) => writeBackRequested = true;

        vm.LoadLyricsAsync(null, json, null, 10f, 0, SuppliedLyricSource.DatabaseCache).Wait();

        Assert.IsFalse(writeBackRequested);
        Assert.AreEqual(RomanizationSchema.Romaji, vm.CurrentRomanizationSchema);
        Assert.HasCount(1, vm.Lyrics);
        Assert.AreEqual("Cached JSON", vm.Lyrics[0].Text);
    }

    [TestMethod]
    public void LyricsViewModel_LoadLyrics_PersistsAutoLoadedLocalLrcAsIntermediateJson()
    {
        var tempDir = Path.Combine(Path.GetTempPath(), Guid.NewGuid().ToString("N"));
        var audioPath = Path.Combine(tempDir, "Local Song.mp3");
        var lyricPath = Path.Combine(tempDir, "Local Song.lrc");
        Directory.CreateDirectory(tempDir);
        string? storedLyric = null;
        int? storedOffset = null;

        try
        {
            File.WriteAllText(audioPath, string.Empty, Encoding.UTF8);
            File.WriteAllText(lyricPath, "[00:01.00]Local", Encoding.UTF8);
            var vm = CreateLyricsViewModel();
            vm.UpdateCurrentLyricRequested += (content, offsetMs) =>
            {
                storedLyric = content;
                storedOffset = offsetMs;
            };

            vm.LoadLyricsAsync(audioPath, null, "Local Song", 10f, 0).Wait();

            Assert.IsNotNull(storedLyric);
            Assert.AreEqual(0, storedOffset);
            using var doc = JsonDocument.Parse(storedLyric);
            Assert.AreEqual(2, doc.RootElement.GetProperty("format_version").GetInt32());
            var line = GetLyricLines(doc).Single();
            Assert.AreEqual("Local", GetRoleLine(line, "lyric").GetProperty("text").GetString());
        }
        finally
        {
            if (Directory.Exists(tempDir))
                Directory.Delete(tempDir, true);
        }
    }

    [TestMethod]
    public void LyricsViewModel_LoadLyrics_PersistsAutoLoadedLocalIntermediateJson()
    {
        var tempDir = Path.Combine(Path.GetTempPath(), Guid.NewGuid().ToString("N"));
        var audioPath = Path.Combine(tempDir, "Local Json.mp3");
        var lyricPath = Path.Combine(tempDir, "Local Json.wplrc");
        const string json = """
                            {
                              "format_version": 1,
                              "offset": 0,
                              "lyric_lines": [
                                {
                                  "time_start_ms": 1000,
                                  "time_end_ms": 2000,
                                  "lines": [
                                    {
                                      "role": "lyric",
                                      "text": "Local JSON"
                                    }
                                  ]
                                }
                              ]
                            }
                            """;
        Directory.CreateDirectory(tempDir);
        string? storedLyric = null;
        int? storedOffset = null;

        try
        {
            File.WriteAllText(audioPath, string.Empty, Encoding.UTF8);
            File.WriteAllText(lyricPath, json, Encoding.UTF8);
            var vm = CreateLyricsViewModel();
            vm.UpdateCurrentLyricRequested += (content, offsetMs) =>
            {
                storedLyric = content;
                storedOffset = offsetMs;
            };

            vm.LoadLyricsAsync(audioPath, null, "Local Json", 10f, 0).Wait();

            Assert.IsNotNull(storedLyric);
            Assert.AreEqual(0, storedOffset);
            using var doc = JsonDocument.Parse(storedLyric);
            Assert.AreEqual(2, doc.RootElement.GetProperty("format_version").GetInt32());
            var line = GetLyricLines(doc).Single();
            Assert.AreEqual("Local JSON", GetRoleLine(line, "lyric").GetProperty("text").GetString());
        }
        finally
        {
            if (Directory.Exists(tempDir))
                Directory.Delete(tempDir, true);
        }
    }

    [TestMethod]
    public void LyricsViewModel_LoadLyrics_DoesNotPersistAutoLoadedLocalV2IntermediateJson()
    {
        var tempDir = Path.Combine(Path.GetTempPath(), Guid.NewGuid().ToString("N"));
        var audioPath = Path.Combine(tempDir, "Local V2 Json.mp3");
        var lyricPath = Path.Combine(tempDir, "Local V2 Json.wplrc");
        const string json = """
                            {
                              "format_version": 2,
                              "romanization_schema": "romaji",
                              "offset": 0,
                              "lyric_lines": [
                                {
                                  "time_start_ms": 1000,
                                  "time_end_ms": 2000,
                                  "lines": [
                                    {
                                      "role": "lyric",
                                      "text": "Local V2 JSON"
                                    }
                                  ]
                                }
                              ]
                            }
                            """;
        Directory.CreateDirectory(tempDir);
        var writeBackRequested = false;

        try
        {
            File.WriteAllText(audioPath, string.Empty, Encoding.UTF8);
            File.WriteAllText(lyricPath, json, Encoding.UTF8);
            var vm = CreateLyricsViewModel();
            vm.UpdateCurrentLyricRequested += (_, _) => writeBackRequested = true;

            vm.LoadLyricsAsync(audioPath, null, "Local V2 Json", 10f, 0).Wait();

            Assert.IsFalse(writeBackRequested);
            Assert.HasCount(1, vm.Lyrics);
            Assert.AreEqual("Local V2 JSON", vm.Lyrics[0].Text);
        }
        finally
        {
            if (Directory.Exists(tempDir))
                Directory.Delete(tempDir, true);
        }
    }

    [TestMethod]
    public void LyricsViewModel_LoadLyrics_RomanjiButtonContentFollowsGlobalSchema()
    {
        const string jyutpingJson = """
                                    {
                                      "format_version": 2,
                                      "romanization_schema": "jyutping",
                                      "offset": 0,
                                      "lyric_lines": [
                                        {
                                          "time_start_ms": 0,
                                          "time_end_ms": 1000,
                                          "lines": [
                                            {
                                              "role": "lyric",
                                              "text": "粤语"
                                            },
                                            {
                                              "role": "romanization",
                                              "text": "jyut ping"
                                            }
                                          ]
                                        }
                                      ]
                                    }
                                    """;
        const string romajiJson = """
                                  {
                                    "format_version": 2,
                                    "romanization_schema": "romaji",
                                    "offset": 0,
                                    "lyric_lines": [
                                      {
                                        "time_start_ms": 0,
                                        "time_end_ms": 1000,
                                        "lines": [
                                          {
                                            "role": "lyric",
                                            "text": "日本語"
                                          },
                                          {
                                            "role": "romanization",
                                            "text": "romaji"
                                          }
                                        ]
                                      }
                                    ]
                                  }
                                  """;

        var jyutpingVm = CreateLyricsViewModel();
        jyutpingVm.LoadLyricsAsync(null, jyutpingJson, null, 10f, 0).Wait();

        Assert.IsTrue(jyutpingVm.HasRomanjiAvailable);
        Assert.AreEqual("拼", jyutpingVm.RomanjiButtonContent);

        var romajiVm = CreateLyricsViewModel();
        romajiVm.LoadLyricsAsync(null, romajiJson, null, 10f, 0).Wait();

        Assert.IsTrue(romajiVm.HasRomanjiAvailable);
        Assert.AreEqual("あ", romajiVm.RomanjiButtonContent);
    }

    [TestMethod]
    public async Task LyricsViewModel_LoadLyrics_InsertsInterludeLineWithoutExportingToWplrc()
    {
        var savePath = Path.Combine(Path.GetTempPath(), $"{Guid.NewGuid():N}.txt");
        var exportedPath = Path.ChangeExtension(savePath, ".wplrc");
        var fileDialogService = new NoopFileDialogService
        {
            SavePath = savePath
        };
        var vm = new LyricsViewModel(
            NullLogger<LyricsViewModel>.Instance,
            fileDialogService,
            CreateLyricParserFactory());
        const string json = """
                            {
                              "format_version": 1,
                              "offset": 0,
                              "lyric_lines": [
                                {
                                  "time_start_ms": 0,
                                  "time_end_ms": 1000,
                                  "lines": [
                                    {
                                      "role": "lyric",
                                      "text": "A"
                                    }
                                  ]
                                },
                                {
                                  "time_start_ms": 7000,
                                  "time_end_ms": 8000,
                                  "lines": [
                                    {
                                      "role": "lyric",
                                      "text": "B"
                                    }
                                  ]
                                }
                              ]
                            }
                            """;

        try
        {
            vm.LoadLyricsAsync(null, json, "Gap Song", 9f, 0).Wait();
            vm.UpdateLyricProgress(4.0f);

            Assert.HasCount(3, vm.Lyrics);
            Assert.AreEqual("♪ ♪ ♪ ♪ ♪ ♪", vm.Lyrics[1].Text);
            Assert.IsTrue(vm.Lyrics[1].IsProgressEnabled);
            Assert.AreEqual(1, vm.CurrentLyricIndex);
            // gap: 2000ms->7000ms
            Assert.AreEqual(0.4, vm.Lyrics[1].Progress, 0.001);

            await vm.ExportWplrcCommand.ExecuteAsync(null);

            var exported = await File.ReadAllTextAsync(exportedPath, Encoding.UTF8);
            Assert.IsFalse(exported.Contains("♪♪♪♪♪♪", StringComparison.Ordinal));
            using var document = JsonDocument.Parse(exported);
            Assert.HasCount(2, GetLyricLines(document));
        }
        finally
        {
            if (File.Exists(savePath))
                File.Delete(savePath);
            if (File.Exists(exportedPath))
                File.Delete(exportedPath);
        }
    }

    [TestMethod]
    public async Task LyricsViewModel_ExportWplrcCommand_WritesPrettyIntermediateJson()
    {
        var savePath = Path.Combine(Path.GetTempPath(), $"{Guid.NewGuid():N}.txt");
        var exportedPath = Path.ChangeExtension(savePath, ".wplrc");
        var fileDialogService = new NoopFileDialogService
        {
            SavePath = savePath
        };
        var vm = new LyricsViewModel(
            NullLogger<LyricsViewModel>.Instance,
            fileDialogService,
            CreateLyricParserFactory());
        const string json = """{"format_version":1,"offset":0,"lyric_lines":[{"time_start_ms":0,"time_end_ms":1000,"lines":[{"role":"lyric","text":"\u4e2d\u6587\u6b4c\u8bcd"}]}]}""";

        Assert.IsFalse(vm.ExportWplrcCommand.CanExecute(null));

        try
        {
            vm.LoadLyricsAsync(null, json, "Export Song", 10f, 0).Wait();

            Assert.IsTrue(vm.ExportWplrcCommand.CanExecute(null));

            await vm.ExportWplrcCommand.ExecuteAsync(null);

            Assert.IsTrue(File.Exists(exportedPath));
            var exported = await File.ReadAllTextAsync(exportedPath, Encoding.UTF8);
            Assert.IsTrue(exported.Contains("\n  \"format_version\": 2", StringComparison.Ordinal));
            StringAssert.Contains(exported, "中文歌词");
            Assert.IsFalse(exported.Contains(@"\u4e2d", StringComparison.OrdinalIgnoreCase));

            using var document = JsonDocument.Parse(exported);
            var lyric = GetRoleLine(GetLyricLines(document).Single(), "lyric");
            Assert.AreEqual("中文歌词", lyric.GetProperty("text").GetString());
        }
        finally
        {
            if (File.Exists(savePath))
                File.Delete(savePath);
            if (File.Exists(exportedPath))
                File.Delete(exportedPath);
        }
    }

    private sealed class NoopFileDialogService : IFileDialogService
    {
        public IReadOnlyList<string> MusicFileExtensions { get; } = [];

        public string? SavePath { get; init; }

        public Task<string?> PickFileAsync(
            IReadOnlyList<string> extensions,
            FileDialogLocation suggestedStartLocation = FileDialogLocation.DocumentsLibrary,
            FileDialogViewMode viewMode = FileDialogViewMode.List) =>
            Task.FromResult<string?>(null);

        public Task<IReadOnlyList<string>> PickFilesAsync(
            IReadOnlyList<string> extensions,
            FileDialogLocation suggestedStartLocation = FileDialogLocation.DocumentsLibrary,
            FileDialogViewMode viewMode = FileDialogViewMode.List) =>
            Task.FromResult<IReadOnlyList<string>>([]);

        public Task<string?> SaveFileAsync(
            IReadOnlyDictionary<string, IReadOnlyList<string>> fileTypeChoices,
            string suggestedFileName,
            FileDialogLocation suggestedStartLocation = FileDialogLocation.DocumentsLibrary) =>
            Task.FromResult(SavePath);

        public Task<string?> PickMusicFileAsync() => Task.FromResult<string?>(null);

        public Task<IReadOnlyList<string>> PickMusicFilesAsync() =>
            Task.FromResult<IReadOnlyList<string>>([]);

        public Task<string?> PickWpplAsync() => Task.FromResult<string?>(null);

        public Task<string?> SaveWpplAsync(string suggestedFileName = "playlist") => Task.FromResult<string?>(null);

        public Task<string?> PickImageAsync() => Task.FromResult<string?>(null);
    }
}
