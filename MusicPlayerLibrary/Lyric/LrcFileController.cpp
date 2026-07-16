// SPDX-License-Identifier: MIT

#include "pch.h"
#include "Lyric/LrcFileController.h"
#include "Core/LocaleConverter.h"
#include "Lyric/MLPipeline/NCNNPipeline.h"

#include <stack>
#include <regex>
#include <ranges>
#include <stdexcept>
#include <string_view>

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <rapidjson/prettywriter.h>

using namespace MusicPlayerLibrary;

namespace
{
std::string TrimWhitespace(std::string_view value)
{
    size_t first = 0;
    while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first])) != 0)
        ++first;

    size_t last = value.size();
    while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1])) != 0)
        --last;

    return std::string(value.substr(first, last - first));
}

std::string TrimChar(std::string_view value, char ch)
{
    size_t first = 0;
    while (first < value.size() && value[first] == ch)
        ++first;

    size_t last = value.size();
    while (last > first && value[last - 1] == ch)
        --last;

    return std::string(value.substr(first, last - first));
}

template <typename TWriter>
void WriteUtf8String(TWriter& writer, const std::string& value)
{
    writer.String(value.data(), static_cast<rapidjson::SizeType>(value.size()));
}

const char* AuxInfoToRole(LrcAuxiliaryInfoNative info)
{
    switch (info)
    {
    case LrcAuxiliaryInfoNative::Lyric: return "lyric";
    case LrcAuxiliaryInfoNative::Translation: return "translation";
    case LrcAuxiliaryInfoNative::Romanization: return "romanization";
    case LrcAuxiliaryInfoNative::Ignored:
    default: return "ignored";
    }
}

const char* LanguageTypeToString(LrcLanguageHelper::LanguageType type)
{
    switch (type)
    {
    case LrcLanguageHelper::LanguageType::zh: return "zh";
    case LrcLanguageHelper::LanguageType::latin: return "latin";
    case LrcLanguageHelper::LanguageType::jp: return "jp";
    case LrcLanguageHelper::LanguageType::kr: return "kr";
    case LrcLanguageHelper::LanguageType::ru: return "ru";
    case LrcLanguageHelper::LanguageType::jyut: return "jyut";
    case LrcLanguageHelper::LanguageType::roma: return "roma";
    case LrcLanguageHelper::LanguageType::onomatopoeia:
    default: return "onomatopoeia";
    }
}

std::string RomanizationSchemaFromClassification(LrcLanguageHelper::LanguageClassification classification)
{
    using LC = LrcLanguageHelper::LanguageClassification;
    switch (classification)
    {
    case LC::zh_jyut: return "jyutping";
    case LC::jp_roma: case LC::jp_zh_trans_roma:
    case LC::kr_roma: case LC::kr_zh_trans_roma: return "romaji";
    default: return std::string{};    
    }
    
}

template <typename TWriter>
void WriteMetadataField(TWriter& writer, const char* key, const std::string& value)
{
    if (value.empty())
        return;

    writer.Key(key);
    WriteUtf8String(writer, value);
}

constexpr double InlineSlashTranslationRatioThreshold = 0.5;
constexpr std::string_view HalfWidthLeftParenthesis = "(";
constexpr std::string_view HalfWidthRightParenthesis = ")";
constexpr std::string_view FullWidthLeftParenthesis = "\xEF\xBC\x88";
constexpr std::string_view FullWidthRightParenthesis = "\xEF\xBC\x89";
constexpr std::string_view FullWidthColon = "\xEF\xBC\x9A";
constexpr std::string_view TranslationMarker = "\xE7\xBF\xBB\xE8\xAF\x91";

bool StartsWith(std::string_view value, std::string_view prefix)
{
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

bool EndsWith(std::string_view value, std::string_view suffix)
{
    return value.size() >= suffix.size() && value.substr(value.size() - suffix.size()) == suffix;
}

bool TryReadParenthesisToken(std::string_view value, size_t position, bool& is_left, size_t& token_size)
{
    const std::string_view remaining = value.substr(position);
    if (StartsWith(remaining, FullWidthLeftParenthesis))
    {
        is_left = true;
        token_size = FullWidthLeftParenthesis.size();
        return true;
    }
    if (StartsWith(remaining, FullWidthRightParenthesis))
    {
        is_left = false;
        token_size = FullWidthRightParenthesis.size();
        return true;
    }
    if (StartsWith(remaining, HalfWidthLeftParenthesis))
    {
        is_left = true;
        token_size = HalfWidthLeftParenthesis.size();
        return true;
    }
    if (StartsWith(remaining, HalfWidthRightParenthesis))
    {
        is_left = false;
        token_size = HalfWidthRightParenthesis.size();
        return true;
    }

    return false;
}

size_t FindMatchingParenthesisClose(std::string_view value, size_t open_position)
{
    bool is_left = false;
    size_t token_size = 0;
    if (!TryReadParenthesisToken(value, open_position, is_left, token_size) || !is_left)
        return std::string_view::npos;

    int depth = 0;
    for (size_t position = open_position; position < value.size();)
    {
        if (!TryReadParenthesisToken(value, position, is_left, token_size))
        {
            ++position;
            continue;
        }

        if (is_left)
        {
            ++depth;
        }
        else
        {
            --depth;
            if (depth == 0)
                return position;
            if (depth < 0)
                return std::string_view::npos;
        }

        position += token_size;
    }

    return std::string_view::npos;
}

bool TrySplitSlashInlineTranslation(const std::string& text, std::string& original, std::string& translation)
{
    constexpr std::string_view delimiter = " / ";
    const size_t delimiter_pos = text.find(delimiter);
    if (delimiter_pos == std::string::npos)
        return false;

    original = TrimWhitespace(std::string_view(text).substr(0, delimiter_pos));
    translation = TrimWhitespace(std::string_view(text).substr(delimiter_pos + delimiter.size()));
    return !original.empty() && !translation.empty();
}

bool TrySplitBracketInlineTranslation(const std::string& text, std::string& original, std::string& translation)
{
    const std::string trimmed_text = TrimWhitespace(text);
    for (size_t position = 0; position < trimmed_text.size();)
    {
        bool is_left = false;
        size_t left_size = 0;
        if (!TryReadParenthesisToken(trimmed_text, position, is_left, left_size))
        {
            ++position;
            continue;
        }
        if (!is_left)
        {
            position += left_size;
            continue;
        }

        const size_t close_position = FindMatchingParenthesisClose(trimmed_text, position);
        if (close_position == std::string_view::npos)
        {
            position += left_size;
            continue;
        }

        bool close_is_left = false;
        size_t close_size = 0;
        if (!TryReadParenthesisToken(trimmed_text, close_position, close_is_left, close_size)
            || close_is_left
            || close_position + close_size != trimmed_text.size())
        {
            position += left_size;
            continue;
        }

        original = TrimWhitespace(std::string_view(trimmed_text).substr(0, position));
        if (original.empty())
        {
            position += left_size;
            continue;
        }

        std::string inner = TrimWhitespace(std::string_view(trimmed_text).substr(
            position + left_size,
            close_position - position - left_size));
        if (!StartsWith(inner, TranslationMarker))
        {
            position += left_size;
            continue;
        }

        std::string after_marker = TrimWhitespace(std::string_view(inner).substr(TranslationMarker.size()));
        size_t colon_size;
        if (StartsWith(after_marker, ":"))
            colon_size = 1;
        else if (StartsWith(after_marker, FullWidthColon))
            colon_size = FullWidthColon.size();
        else
        {
            position += left_size;
            continue;
        }

        translation = TrimWhitespace(std::string_view(after_marker).substr(colon_size));
        if (!translation.empty())
            return true;

        position += left_size;
    }

    return false;
}

std::vector<std::string> SplitInlineTranslationText(const std::string& text, bool slash_translation_enabled)
{
    std::string original;
    std::string translation;
    if (TrySplitBracketInlineTranslation(text, original, translation)
        || (slash_translation_enabled && TrySplitSlashInlineTranslation(text, original, translation)))
    {
        return { original, translation };
    }

    return { text };
}
}

template <typename T>
static int FindVectorIndex(const std::vector<T>& values, const T& value)
{
    auto it = std::ranges::find(values, value);
    return it == values.end() ? -1 : static_cast<int>(std::distance(values.begin(), it));
}

static std::regex time_tag_regex(R"(\[\s*(\d{1,2})\s*[:.]\s*(\d{1,2})(?:\s*[:.]\s*(\d{1,4}))?\s*\])");
static std::regex time_tag_progress_regex(R"(\s*(\d{1,2})\s*[:.]\s*(\d{1,2})(?:\s*[:.]\s*(\d{1,4}))?\s*)");

std::string SplitLrcForProgressMultiNode1(const std::string& text) 
{
    std::string new_text;
    bool is_pressed = false;
    for (size_t j = 0; j < text.size(); ++j)
    {
        if (text[j] == '[' || text[j] == '<')
        {
            is_pressed = true;
            continue;
        }
        if (text[j] == ']' || text[j] == '>')
        {
            is_pressed = false;
        }
        else
        {
            if (is_pressed) continue;
            new_text.push_back(text[j]);
        }
    }
    return new_text;
}

std::vector<std::string> SplitLrcForProgressMultiNode2(const std::vector<std::string>& texts)
{
    std::vector<std::string> strs;
    strs.reserve(texts.size());
    for (const auto& text : texts)
    {
        strs.push_back(SplitLrcForProgressMultiNode1(text));
    }
    return strs;
}

LrcMultiNode::LrcMultiNode(int t, const std::vector<std::string>& texts, 
    LrcLanguageHelper::LanguageClassification classification,
    const std::vector<LrcLanguageHelper::LanguageType>& recommend_slot) :
    LrcAbstractNode(t), str_count(static_cast<int>(texts.size())), lrc_texts(texts)
{
    for (int i = 0; i < str_count; ++i)
    {
        aux_infos.push_back(LrcAuxiliaryInfoNative::Ignored);
    }
    int jp_index = -1, kr_index = -1, latin_index = -1, zh_index = -1, ru_index = -1, jyut_index = -1, roma_index = -1, onomatopoeia_index = -1;
    using LC = LrcLanguageHelper::LanguageClassification;
    if (recommend_slot.size() == texts.size())
    {
        lang_types = recommend_slot;
        for (int i = 0; i < static_cast<int>(recommend_slot.size()); ++i)
        {
            switch (recommend_slot[i])
            {
            case LrcLanguageHelper::LanguageType::jp: if (jp_index == -1) jp_index = i; break;
            case LrcLanguageHelper::LanguageType::kr: if (kr_index == -1) kr_index = i; break;
            case LrcLanguageHelper::LanguageType::latin: if (latin_index == -1) latin_index = i; break;
            case LrcLanguageHelper::LanguageType::zh: if (zh_index == -1) zh_index = i; break;
            case LrcLanguageHelper::LanguageType::ru: if (ru_index == -1) ru_index = i; break;
            case LrcLanguageHelper::LanguageType::jyut: if (jyut_index == -1) jyut_index = i; break;
            case LrcLanguageHelper::LanguageType::roma: if (roma_index == -1) roma_index = i; break;
            default: if (onomatopoeia_index == -1) onomatopoeia_index = i; break;
            }
        }
    }
    else
    {
        for (int i = 0; i < str_count; ++i)
        {
            lang_types.push_back(LrcLanguageHelper::GetSingleton().detect_line_language_type(texts[i]));
        }
        jp_index = FindVectorIndex(lang_types, LrcLanguageHelper::LanguageType::jp), 
        kr_index = FindVectorIndex(lang_types, LrcLanguageHelper::LanguageType::kr),
        latin_index = FindVectorIndex(lang_types, LrcLanguageHelper::LanguageType::latin),
        ru_index = FindVectorIndex(lang_types, LrcLanguageHelper::LanguageType::ru),
        zh_index = FindVectorIndex(lang_types, LrcLanguageHelper::LanguageType::zh),
        jyut_index = FindVectorIndex(lang_types, LrcLanguageHelper::LanguageType::jyut),
        roma_index = FindVectorIndex(lang_types, LrcLanguageHelper::LanguageType::roma),
        onomatopoeia_index = FindVectorIndex(lang_types, LrcLanguageHelper::LanguageType::onomatopoeia);
    }
    auto assign_with_language = [&](int index, LrcAuxiliaryInfoNative type)
    {
        if (index != -1) aux_infos[index] = type;
    };
    switch (classification)
    {
    case LC::latin_only:
        {
            assign_with_language(latin_index, LrcAuxiliaryInfoNative::Lyric);
            break;
        }
    case LC::jp_only:
        {
            assign_with_language(jp_index, LrcAuxiliaryInfoNative::Lyric);
            break;
        }
    case LC::zh_only:
        {
            assign_with_language(zh_index, LrcAuxiliaryInfoNative::Lyric);
            break;
        }
    case LC::kr_only:
        {
            assign_with_language(kr_index, LrcAuxiliaryInfoNative::Lyric); 
            break;
        }
    case LC::ru_only:
        {
            assign_with_language(ru_index, LrcAuxiliaryInfoNative::Lyric);
            break;
        }
    case LC::zh_jyut:
        {
            assign_with_language(zh_index, LrcAuxiliaryInfoNative::Lyric);
            assign_with_language(jyut_index, LrcAuxiliaryInfoNative::Romanization);
            if (jyut_index == -1)
            {
                if (latin_index != -1)
                    assign_with_language(latin_index, LrcAuxiliaryInfoNative::Romanization);
                if (roma_index != -1)
                    assign_with_language(roma_index, LrcAuxiliaryInfoNative::Romanization);
                else
                    assign_with_language(onomatopoeia_index, LrcAuxiliaryInfoNative::Romanization);
            }
            break;
        }
    case LC::jp_roma:
        {
            assign_with_language(jp_index, LrcAuxiliaryInfoNative::Lyric);
            assign_with_language(roma_index, LrcAuxiliaryInfoNative::Romanization);
            if (roma_index == -1)
            {
                assign_with_language(onomatopoeia_index, LrcAuxiliaryInfoNative::Romanization);
            }
            if (jp_index == -1)
            {
                if (zh_index != -1)
                {
                    assign_with_language(zh_index, LrcAuxiliaryInfoNative::Lyric);
                }
                else if (latin_index != -1)
                {
                    assign_with_language(latin_index, LrcAuxiliaryInfoNative::Lyric);
                }
            }
            break;
        }
    case LC::kr_roma:
        {
            assign_with_language(kr_index, LrcAuxiliaryInfoNative::Lyric);
            assign_with_language(roma_index, LrcAuxiliaryInfoNative::Romanization);
            if (kr_index == -1)
            {
                if (zh_index != -1)
                {
                    assign_with_language(zh_index, LrcAuxiliaryInfoNative::Lyric);
                }
                else if (latin_index != -1)
                {
                    assign_with_language(latin_index, LrcAuxiliaryInfoNative::Lyric);
                }
            }
            break;
        }
    case LC::latin_zh_trans:
        {
            assign_with_language(latin_index, LrcAuxiliaryInfoNative::Lyric);
            assign_with_language(zh_index, LrcAuxiliaryInfoNative::Translation);
            if (latin_index == -1)
            {
                if (jp_index != -1)
                    assign_with_language(jp_index, LrcAuxiliaryInfoNative::Lyric);
                else if (kr_index != -1)
                    assign_with_language(kr_index, LrcAuxiliaryInfoNative::Lyric);
                else if (jyut_index != -1)
                    assign_with_language(jyut_index, LrcAuxiliaryInfoNative::Lyric);
                else
                    assign_with_language(onomatopoeia_index, LrcAuxiliaryInfoNative::Lyric);
            }
            break;
        }
    case LC::ru_zh_trans:
        {
            assign_with_language(ru_index, LrcAuxiliaryInfoNative::Lyric);
            assign_with_language(zh_index, LrcAuxiliaryInfoNative::Translation);
            if (ru_index == -1)
            {
                if (jp_index != -1)
                    assign_with_language(jp_index, LrcAuxiliaryInfoNative::Lyric);
                else if (kr_index != -1)
                    assign_with_language(kr_index, LrcAuxiliaryInfoNative::Lyric);
                else if (jyut_index != -1)
                    assign_with_language(jyut_index, LrcAuxiliaryInfoNative::Lyric);
                else if (latin_index != -1)
                    assign_with_language(latin_index, LrcAuxiliaryInfoNative::Lyric);
                else
                    assign_with_language(onomatopoeia_index, LrcAuxiliaryInfoNative::Lyric);
            }
            break;
        }
    case LC::jp_zh_trans:
        {
            assign_with_language(jp_index, LrcAuxiliaryInfoNative::Lyric);
            assign_with_language(zh_index, LrcAuxiliaryInfoNative::Translation);
            if (jp_index == -1)
            {
                if (zh_index != -1)
                {
                    int zh_index_2 = -1;
                    for (int i = zh_index + 1; i < static_cast<int>(texts.size()); i++)
                    {
                        if (lang_types[i] == LrcLanguageHelper::LanguageType::zh)
                        {
                            zh_index_2 = i; break;
                        }
                    }
                    if (zh_index_2 != -1)
                    {
                        assign_with_language(zh_index, LrcAuxiliaryInfoNative::Lyric);
                        assign_with_language(zh_index_2, LrcAuxiliaryInfoNative::Translation);
                    }
                    else if (latin_index != -1)
                    {
                        assign_with_language(latin_index, LrcAuxiliaryInfoNative::Lyric);
                        assign_with_language(zh_index, LrcAuxiliaryInfoNative::Translation);
                    }
                    else if (onomatopoeia_index != -1)
                    {
                        assign_with_language(onomatopoeia_index, LrcAuxiliaryInfoNative::Lyric);
                    }
                    else
                    {
                        assign_with_language(zh_index, LrcAuxiliaryInfoNative::Lyric);
                    }
                }
                else if (latin_index != -1)
                {
                    assign_with_language(latin_index, LrcAuxiliaryInfoNative::Lyric);
                }
                else
                {
                    assign_with_language(onomatopoeia_index, LrcAuxiliaryInfoNative::Lyric);
                }
            } 
            else if (zh_index == -1)
            {
                if (latin_index != -1)
                    assign_with_language(latin_index, LrcAuxiliaryInfoNative::Translation);
            }
            break;
        }
    case LC::kr_zh_trans:
        {
            assign_with_language(kr_index, LrcAuxiliaryInfoNative::Lyric);
            assign_with_language(zh_index, LrcAuxiliaryInfoNative::Translation);
            if (kr_index == -1)
            {
                if (zh_index != -1)
                {
                    assign_with_language(zh_index, LrcAuxiliaryInfoNative::Lyric);
                }
                else if (latin_index != -1)
                {
                    assign_with_language(latin_index, LrcAuxiliaryInfoNative::Lyric);
                }
                else
                {
                    assign_with_language(onomatopoeia_index, LrcAuxiliaryInfoNative::Lyric);
                }
            }
            break;
        }
    case LC::jp_zh_trans_roma:
        {
            assign_with_language(jp_index, LrcAuxiliaryInfoNative::Lyric);
            assign_with_language(zh_index, LrcAuxiliaryInfoNative::Translation);
            assign_with_language(roma_index, LrcAuxiliaryInfoNative::Romanization);
            if (jp_index == -1)
            {
                if (zh_index != -1)
                {
                    int zh_index_2 = -1;
                    for (int i = zh_index + 1; i < static_cast<int>(texts.size()); i++)
                    {
                        if (lang_types[i] == LrcLanguageHelper::LanguageType::zh)
                        {
                            zh_index_2 = i; break;
                        }
                    }
                    if (zh_index_2 != -1)
                    {
                        assign_with_language(zh_index, LrcAuxiliaryInfoNative::Lyric);
                        assign_with_language(zh_index_2, LrcAuxiliaryInfoNative::Translation);
                    }
                    if (roma_index == -1)
                    {
                        // 信任ML分类器产生的eng识别结果，假定其为歌词。
                        if (latin_index != -1)
                        {
                            assign_with_language(latin_index, LrcAuxiliaryInfoNative::Lyric);
                            assign_with_language(zh_index, LrcAuxiliaryInfoNative::Translation);
                        }
                        else
                        {
                            assign_with_language(zh_index, LrcAuxiliaryInfoNative::Lyric);
                            assign_with_language(onomatopoeia_index, LrcAuxiliaryInfoNative::Romanization);
                        }
                    }
                    else
                    {
                        assign_with_language(zh_index, LrcAuxiliaryInfoNative::Lyric);
                        assign_with_language(roma_index, LrcAuxiliaryInfoNative::Romanization);
                    }
                }
                else if (latin_index != -1)
                {
                    assign_with_language(latin_index, LrcAuxiliaryInfoNative::Lyric);
                    assign_with_language(onomatopoeia_index, LrcAuxiliaryInfoNative::Romanization);
                }
            }
            else if (roma_index == -1)
            {
                if (latin_index != -1)
                    assign_with_language(latin_index, LrcAuxiliaryInfoNative::Romanization);
                else
                    assign_with_language(onomatopoeia_index, LrcAuxiliaryInfoNative::Romanization);
            }
            break;
        }
    case LC::kr_zh_trans_roma:
        {
            assign_with_language(kr_index, LrcAuxiliaryInfoNative::Lyric);
            assign_with_language(zh_index, LrcAuxiliaryInfoNative::Translation);
            assign_with_language(roma_index, LrcAuxiliaryInfoNative::Romanization);
            if (kr_index == -1)
            {
                if (zh_index != -1)
                {
                    assign_with_language(zh_index, LrcAuxiliaryInfoNative::Lyric);
                }
                else if (latin_index != -1)
                {
                    assign_with_language(latin_index, LrcAuxiliaryInfoNative::Lyric);
                }
            }
            if (roma_index == -1)
            {
                assign_with_language(onomatopoeia_index, LrcAuxiliaryInfoNative::Romanization);
            }
            break;
        }
    default:
        break;
    }
}

LrcLanguageHelper::LrcLanguageHelper()
{
    auto line_model_file = MLPipeline::NcnnModelFiles{
        .param = L"lyric_lang_mlp.ncnn.param",
        .weights = L"lyric_lang_mlp.ncnn.bin"
    };
    auto song_model_file = MLPipeline::NcnnModelFiles{
        .param = L"song_structure_mlp.ncnn.param",
        .weights = L"song_structure_mlp.ncnn.bin"
    };

    line_net_reasoning.reset(new MLPipeline::NcnnClassifier(line_model_file)) ;
    line_vocab_reasoning = MLPipeline::load_vocab(
        L"lyric_lang_vocab.bin",
        line_net_reasoning->model_fingerprint());
    if (line_vocab_reasoning.size() != line_net_reasoning->input_size())
    {
        throw std::runtime_error(
            "vocabulary has " + std::to_string(line_vocab_reasoning.size()) +
            " entries; NCNN model expects " + std::to_string(line_net_reasoning->input_size()));
    }
    song_net_reasoning.reset(new MLPipeline::NcnnClassifier(song_model_file));
    
    if (!line_net_reasoning || !song_net_reasoning)
    {
        throw std::runtime_error("NCNN initialization failed.");
    }
}

LrcLanguageHelper::~LrcLanguageHelper()
{
    release_native_resources();
}

void LrcLanguageHelper::release_native_resources() noexcept
{
    accepting_inference.store(false, std::memory_order_release);
    std::lock_guard lock(dlib_mutex);
    song_net_reasoning.reset();
    line_net_reasoning.reset();
    line_vocab_reasoning.clear();
}

std::string LrcLanguageHelper::lyric_type_to_std_string(LanguageType type)
{
    switch (type)
    {
    case LanguageType::zh: return "zh";
    case LanguageType::latin: return "latin";
    case LanguageType::jp: return "jp";
    case LanguageType::kr: return "kr";
    case LanguageType::ru: return "ru";
    case LanguageType::jyut: return "jyut";
    case LanguageType::roma: return "roma";
    default: return "onomatopoeia";
    }
}

std::vector<double> LrcLanguageHelper::extract_line_features(const std::string& text,
                                                             const MLPipeline::Vocabulary& vocab)
{
    std::vector x(vocab.size(), 0.0);

    for (size_t i = 0; i < text.size(); ++i)
    {
        for (int n = 1; n <= 3; ++n)
        {
            if (i + n > text.size()) continue;
            std::string gram = text.substr(i, n);
            auto it = vocab.find(gram);
            if (it != vocab.end())
                x[it->second] += 1.0;
        }
    }
    return x;
}

std::vector<float> LrcLanguageHelper::to_float_features(const std::vector<double>& features)
{
    return { features.begin(), features.end() };
}

LrcLanguageHelper::LanguageType
LrcLanguageHelper::detect_line_language_type(const std::string& input)
{
    std::lock_guard lock(dlib_mutex);
    if (!accepting_inference.load(std::memory_order_acquire) || !line_net_reasoning)
        throw std::runtime_error("native lyric inference is shutting down");

    auto feat = extract_line_features(input, line_vocab_reasoning);
    const auto features = to_float_features(feat);

    switch (int label_id = line_net_reasoning->predict(features); label_id)
    {
    case 0: return LanguageType::zh;
    case 1: return LanguageType::jp;
    case 2: return LanguageType::kr;
    case 3: return LanguageType::latin;
    case 4: return LanguageType::ru;
    case 5: return LanguageType::jyut;
    case 6: return LanguageType::roma;
    case 7: default: return LanguageType::onomatopoeia;
    }
}

song_sample_type 
LrcLanguageHelper::extract_song_features(const std::vector<LrcLanguageHelper::LanguageType>& seq)
{
    using LT = LrcLanguageHelper::LanguageType;
    const int LANGS = 8; // zh jp kr en jyut roma ono
    song_sample_type feat(84, 1); 
    feat.clear();

    // language count map
    std::vector count(LANGS, 0);
    for (auto& s : seq)
    {
        if (s == LT::zh) count[0]++;
        else if (s == LT::jp) count[1]++;
        else if (s == LT::kr) count[2]++;
        else if (s == LT::latin) count[3]++;
        else if (s == LT::ru) count[4]++;
        else if (s == LT::jyut) count[5]++;
        else if (s == LT::roma) count[6]++;
        else count[7]++; // onomatopoeia
        
    }

    int idx = 0;

    // language count (8 rows)
    for (int i = 0; i < LANGS; i++)
        feat.push_back(count[i]);

    // language proportion (8 rows)
    const double total = static_cast<double>(seq.size());
    for (int i = 0; i < LANGS; i++)
        feat.push_back(count[i] / total);

    // bigram matrix (64 rows)
    std::vector trans(LANGS, std::vector(LANGS, 0));
    for (size_t i = 1; i < seq.size(); i++)
    {
        auto prev = seq[i - 1];
        auto curr = seq[i];

        auto id = [&](LT s) {
            if (s == LT::zh) return 0;
            if (s == LT::jp) return 1;
            if (s == LT::kr) return 2;
            if (s == LT::latin) return 3;
            if (s == LT::ru) return 4;
            if (s == LT::jyut) return 5;
            if (s == LT::roma) return 6;
            return 7;
        };

        trans[id(prev)][id(curr)]++;
    }

    for (int i = 0; i < LANGS; i++)
        for (int j = 0; j < LANGS; j++)
            feat.push_back(trans[i][j]);

    // language switching count (1 row)
    int switches = 0;
    for (size_t i = 1; i < seq.size(); i++)
        if (seq[i] != seq[i - 1])
            switches++;
    feat.push_back(switches);

    // translation included? (1 row)
    feat.push_back(count[0] > 0 && 
        (count[1] > 0 
            || count[2] > 0 
            || count[3] > 0 
            || count[4] > 0));

    // jyutping included? (1 row)
    feat.push_back(count[5] > 0);

    // romaji included? (1 row)
    feat.push_back(count[6] > 0);

    return feat;
}

LrcLanguageHelper::LanguageClassification LrcLanguageHelper::detect_song_language_classification(
    const std::vector<LrcLanguageHelper::LanguageType>& lyric_lang_type)
{
    auto song_feat = extract_song_features(lyric_lang_type);
    int reasoning_result;
    {
        std::lock_guard lock(dlib_mutex);
        if (!accepting_inference.load(std::memory_order_acquire) || !song_net_reasoning)
            throw std::runtime_error("native lyric inference is shutting down");
        auto features = to_float_features(song_feat);
        reasoning_result = song_net_reasoning->predict(features);
    }
    static std::unordered_map<LanguageClassification, unsigned long> table = {
        {LanguageClassification::zh_only, 0},
        {LanguageClassification::jp_only, 1},
        {LanguageClassification::kr_only, 2},
        {LanguageClassification::latin_only, 3},
        {LanguageClassification::ru_only, 4},
        {LanguageClassification::jp_zh_trans, 5},
        {LanguageClassification::jp_roma, 6},
        {LanguageClassification::latin_zh_trans, 7},
        {LanguageClassification::ru_zh_trans, 8},
        {LanguageClassification::kr_zh_trans, 9},
        {LanguageClassification::kr_roma, 10},
        {LanguageClassification::zh_jyut, 11},
        {LanguageClassification::jp_zh_trans_roma, 12},
        {LanguageClassification::kr_zh_trans_roma, 13}
    };
    
    auto it = std::ranges::find_if(table, [reasoning_result](const std::pair<LanguageClassification, unsigned long>& key) -> bool {
        return key.second == reasoning_result;
    });
    if (it != table.end())
        return it->first;
    return LanguageClassification::latin_only;
}

auto LrcLanguageHelper::detect_language_slot(
    const std::vector<std::vector<LanguageType>>& lines) -> std::vector<LanguageType>
{
    // language序列转int，不固定
    std::unordered_map<int, std::vector<LanguageType>> slot_map_table;
    // 桶，记录每个序列的得分
    std::unordered_map<int, int> slot_bucket;
    
    int lang_id = 0;
    for (const auto& line: lines)
    {
        int this_lang_id;
        auto it = std::ranges::find_if(slot_map_table, [line](const std::pair<int, std::vector<LanguageType>>& key)
        {
            return key.second == line;
        });
        if (it == slot_map_table.end())
        {
            this_lang_id = lang_id;
            slot_map_table[lang_id++] = line;
        }
        else
        {
            this_lang_id = it->first;
        }
        slot_bucket[this_lang_id]++;
    }
    auto max_it = std::ranges::max_element(slot_bucket,
        [](const auto& a, const auto& b) {
            return a.second < b.second; // 按 value 比较
        });

    if (max_it == slot_bucket.end())
        return {};

    int best_lang_id = max_it->first;
    const auto& best_slot_type = slot_map_table[best_lang_id];
    std::string best_slot_debug_str = "[ ";
    for (const auto& slot : best_slot_type)
    {
        best_slot_debug_str += lyric_type_to_std_string(slot) + " ";
    }
    best_slot_debug_str += "]";
    NATIVE_TRACE("info: detect slot type = %s", best_slot_debug_str.c_str());
    return best_slot_type;
}

LrcLanguageHelper& LrcLanguageHelper::GetSingleton()
{
    static LrcLanguageHelper helper_instance;
    return helper_instance;
}

void LrcLanguageHelper::InitializeSingleton()
{
    (void)GetSingleton();
}

void LrcLanguageHelper::ShutdownSingleton() noexcept
{
    GetSingleton().release_native_resources();
}

LrcLanguageHelper::LanguageType LrcAbstractNode::get_language_type(int index) const
{
    std::string text;
    if (get_lrc_str_at(index, text) != 0)
        return LrcLanguageHelper::LanguageType::onomatopoeia;

    return LrcLanguageHelper::GetSingleton().detect_line_language_type(text);
}

LrcLanguageHelper::LanguageType LrcMultiNode::get_language_type(int index) const
{
    if (index >= 0 && static_cast<size_t>(index) < lang_types.size())
        return lang_types[index];

    return LrcAbstractNode::get_language_type(index);
}

LrcProgressNode::LrcProgressNode(int t, const std::string& text_with_node, int controller_line_index)
    : LrcAbstractNode(t), node_count(0), end_time_ms(0), controller_line_index(controller_line_index)
{
    std::string text = text_with_node;
    const char right_brace_type = text[text.size() - 1];
    char left_brace_type;
    switch (right_brace_type)
    {
        case ']': left_brace_type = '['; break;
        case '>': left_brace_type = '<'; break;
        default: return;
    }
    while (true)
    {
        const size_t right_brace_index = text.find(right_brace_type);
        if (right_brace_index == std::string::npos)
            break;

        std::string node = text.substr(0, right_brace_index + 1);
        const size_t node_controller_start_index = node.find(left_brace_type);
        if (node_controller_start_index == std::string::npos)
        {
            NATIVE_TRACE("warn: invalid progress node: %s\n", node.c_str());
            break;
        }
        auto time_stamp = node.substr(node_controller_start_index + 1);
        std::erase(time_stamp, right_brace_type);
        auto lyric_text = node.substr(0, node_controller_start_index);
        std::smatch m;
        if (std::regex_search(time_stamp, m, time_tag_progress_regex)
            && m.size() == 4)
        {
            int minutes = std::stoi(m[1].str());
            int seconds = std::stoi(m[2].str());
            std::string milliseconds_str = m[3].str();
            int milliseconds = std::stoi(milliseconds_str);
            if (milliseconds_str.size() < 3)
            {
                auto multiples = 3 - milliseconds_str.size();
                milliseconds *= static_cast<int>(std::floor(pow(10, multiples)));
            }
            if (milliseconds_str.size() >= 4)
            {
                auto multiples = milliseconds_str.size() - 3;
                milliseconds /= static_cast<int>(std::floor(pow(10, multiples)));
            }
            int total_ms = minutes * 60000 + seconds * 1000 + milliseconds;
            if (total_ms < 0) total_ms = 0;
            nodes.push_back({
                .time_ms = total_ms,
                .node_text = lyric_text
            });
            node_count++;
        }
        text.erase(0, right_brace_index + 1);
    }
}

int LrcProgressNode::get_intrinsic_end_time_ms() const
{
    if (end_time_ms > 0)
        return end_time_ms;

    if (!nodes.empty())
        return nodes.back().time_ms;

    return time_ms;
}

int LrcProgressNode::get_controller_node_count(int line_index) const
{
    if (line_index != controller_line_index)
        return 0;

    int count = 0;
    for (const auto& node : nodes)
    {
        if (!node.node_text.empty())
            ++count;
    }
    return count;
}

int LrcProgressNode::get_controller_node_at(
    int line_index,
    int node_index,
    int& start_time_ms,
    int& end_time_ms,
    std::string& out_str) const
{
    if (line_index != controller_line_index || node_index < 0)
        return -1;

    int visible_index = 0;
    for (int i = 0; i < static_cast<int>(nodes.size()); ++i)
    {
        if (nodes[i].node_text.empty())
            continue;

        if (visible_index == node_index)
        {
            start_time_ms = i == 0 ? time_ms : nodes[i - 1].time_ms;
            end_time_ms = nodes[i].time_ms;
            out_str = nodes[i].node_text;
            return 0;
        }
        ++visible_index;
    }

    return -1;
}

int SelectBestControllerLine(const std::vector<std::string>& str_arr)
{
    // 控制点越多，该行为控制行的置信度越高
    std::vector controller_bucket(str_arr.size(), 0);
    for (int i = 0; i < static_cast<int>(str_arr.size()); ++i)
    {
        const auto& text = str_arr[i];
        bool is_pressed = false;
        for (size_t j = 0; j < text.size(); ++j)
        {
            if (text[j] == '[' || text[j] == '<')
            {
                is_pressed = true;
                continue;
            }
            if (text[j] == ']' || text[j] == '>')
            {
                is_pressed = false;
                controller_bucket[i]++;
            }
            else
            {
                if (is_pressed) continue;
            }
        }
    }
    auto max_it = std::ranges::max_element(controller_bucket);
    int max_index = static_cast<int>(std::distance(controller_bucket.begin(), max_it));
    return max_index;
}

LrcProgressMultiNode::LrcProgressMultiNode
    (int t, const std::vector<std::string>& str_arr_2, 
        LrcLanguageHelper::LanguageClassification classification, 
        std::vector<LrcLanguageHelper::LanguageType> recommend_slot):
    LrcAbstractNode(t),
    LrcMultiNode(t, SplitLrcForProgressMultiNode2(str_arr_2), classification, recommend_slot),
    LrcProgressNode(t, str_arr_2[SelectBestControllerLine(str_arr_2)], SelectBestControllerLine(str_arr_2)) { }

LrcFileController::LrcFileController(int song_end_time_ms)
    : song_end_time_ms(song_end_time_ms)
{
}

LrcFileController::~LrcFileController()
{
    clear_lrc_nodes();
}

LrcAbstractNode* LrcNodeFactory::CreateLrcNode(
    int time_ms, const std::vector<std::string>& lrc_texts, 
    LrcLanguageHelper::LanguageClassification classification,
    std::vector<LrcLanguageHelper::LanguageType> recommend_slot)
{
    auto ifLrcContainsControllerNode = [](const std::string& lrc_text)
        {
            if (lrc_text.empty())
                return false;
            const auto last_index = lrc_text.size() - 1;
            return
                (lrc_text[last_index] == ']' || lrc_text[last_index] == '>');
        };
    if (lrc_texts.size() == 1) {
        if (ifLrcContainsControllerNode(lrc_texts[0]))
            return new LrcProgressNode(time_ms, lrc_texts[0]);
        return new LrcNode(time_ms, lrc_texts[0]);
    }
    // 遍历所有行，选取第一个有控制点的行构造LrcProgressMultiNode；若没有控制点，回退到LrcMultiNode
    if (lrc_texts.size() > 1) {
        for (int i = 0; i < static_cast<int>(lrc_texts.size()); ++i)
            if (ifLrcContainsControllerNode(lrc_texts[i]))
                return new LrcProgressMultiNode(time_ms, lrc_texts, classification, recommend_slot);
        return new LrcMultiNode(time_ms, lrc_texts, classification, recommend_slot);
    }
    return nullptr;
}

void LrcFileController::parse_lrc_file_stream(IFile* file_stream)
{
    // 当前支持的格式：
    // 逐行LRC，逐字LRC，Extended LRC，交错翻译，同步翻译
    if (file_stream == nullptr)
    {
        return;
    }
    clear_lrc_nodes();
    metadata = {};
    lrc_offset_ms = 0;
    std::string file_content_bytes;
    const int buf_size = 4096;
    char buffer[buf_size];
    uint32_t bytes_read = 0;
    do
    {
        bytes_read = file_stream->Read(buffer, buf_size - 1);
        buffer[bytes_read] = '\0';
        file_content_bytes.append(buffer, bytes_read);
    }
    while (bytes_read > 0);
    const std::string file_content_utf8 =
        LocaleConverter::GetUtf8StringFromBytes(file_content_bytes.data(), file_content_bytes.size());

    // fix issue #12
    // 关于歌词文件/歌曲内嵌歌词内出现时间tag非强制有序的翻译歌词时程序的错误/闪退问题
    // struct definition: caching each line for strong_ordering sort
    struct CachedTimeLine
    {
        int time_stamp_ms;
        std::string text;
    };
    std::vector<CachedTimeLine> time_lines;
    
    // 逐行解析
    size_t start = 0;
    int flag_decoding_metadata = 1;
    std::stack<std::string> lyrics_in_ms;
    int recorded_ms = 0;

    while (start < file_content_utf8.size())
    {
        size_t end = file_content_utf8.find('\n', start);
        if (end == std::string::npos)
        {
            end = file_content_utf8.size();
            // 因为现在缓存所有歌词行，所以不需要设置is_lrc_end flag
        }
        std::string line = TrimWhitespace(std::string_view(file_content_utf8).substr(start, end - start));
        if (line.empty())
        {
            start = end + 1;
            continue;
        }
        if (line[0] == '{')
        {
            NATIVE_TRACE("warn: invalid ncm extension found, ignoring\n");
            start = end + 1;
            continue;
        }
        const size_t line_start_index = line.find('[');
        // 剔除行开头的不合法字符
        if (line_start_index != std::string::npos && line_start_index != 0)
        {
            NATIVE_TRACE("warn: invalid lrc format, ignoring start character: %s\n",
                line.substr(0, line_start_index).c_str());
            line.erase(0, line_start_index);
        }

        // fix: moving decode_metadata as a lambda
        auto decode_metadata = [](const std::string& line, decltype(metadata)& meta, int& offset) -> int {
            // 走metadata解析，不遵守标准lrc解码
            switch (get_metadata_type(line))
            {
            case LrcMetadataTypeNative::Artist:
                meta.artist = get_metadata_value(line);
                break;
            case LrcMetadataTypeNative::Album:
                meta.album = get_metadata_value(line);
                break;
            case LrcMetadataTypeNative::Title:
                meta.title = get_metadata_value(line);
                break;
            case LrcMetadataTypeNative::By:
                meta.by = get_metadata_value(line);
                break;
            case LrcMetadataTypeNative::Offset:
                offset = static_cast<int>(std::strtol(get_metadata_value(line).c_str(), nullptr, 10));
                break;
            case LrcMetadataTypeNative::Author:
                meta.author = get_metadata_value(line);
                break;
            case LrcMetadataTypeNative::Ignored:
                break;
            case LrcMetadataTypeNative::Error: default:
                return -1;
            }
            return 0;
        };
        if (flag_decoding_metadata)
        {
            if (decode_metadata(line, metadata, lrc_offset_ms) == 0)
            {
                start = end + 1;
            }
            else {
                flag_decoding_metadata = false;
            }
            continue;
        }
        // 解析时间tag
        if (line.size() < 7) // 现在能解析的最小tag为7位(如：[11:45]
        {
            clear_lrc_nodes();
			throw std::runtime_error("Invalid lrc line, aborting!");
        }

        std::string lyric_text = line;
        // 处理同一行多个时间戳的问题
        std::vector<int> time_stamps;
        while (!lyric_text.empty() && lyric_text[0] == '[')
        {
            const size_t time_tag_end_index_multi = lyric_text.find(']');
            if (time_tag_end_index_multi == std::string::npos)
				throw std::runtime_error("Invalid lrc time tag, aborting!");
            auto time_tag = lyric_text.substr(0, time_tag_end_index_multi + 1);
            std::smatch m;
            bool is_malformed_time_tag = true;
            if (std::regex_search(time_tag, m, time_tag_regex))
            {
                is_malformed_time_tag = m.size() != 4;
            }
            if (is_malformed_time_tag)
            {
                // malformed time tag
                // guess: metadata tag?
                // fix issue #12
                auto metadata_substr = lyric_text.substr(0, time_tag_end_index_multi + 1);
                decode_metadata(metadata_substr, metadata, lrc_offset_ms);
                lyric_text = TrimWhitespace(std::string_view(lyric_text).substr(time_tag_end_index_multi + 1));
                continue;
            }
            // fix issue #51, problem H2
            // previously, empty milliseconds part can break the decoding pipeline.
            // proceed with std::invalid_argument manually.
            auto try_to_stoi = [](const std::string& str, const int default_value = 0)
            {
                try { return std::stoi(str); }
                catch (std::invalid_argument) { return default_value; }
            };
            int minutes = try_to_stoi(m[1].str());
            int seconds = try_to_stoi(m[2].str());
            std::string milliseconds_str = m[3].str();
            int milliseconds = try_to_stoi(milliseconds_str);
            if (milliseconds != 0)
            {
                if (milliseconds_str.size() < 3)
                {
                    auto multiples = 3 - milliseconds_str.size();
                    milliseconds *= static_cast<int>(std::floor(pow(10, multiples)));
                }
                if (milliseconds_str.size() >= 4)
                {
                    auto multiples = milliseconds_str.size() - 3;
                    milliseconds /= static_cast<int>(std::floor(pow(10, multiples)));
                }
            }
            int total_ms_multi = minutes * 60000 + seconds * 1000 + milliseconds;
            if (total_ms_multi < 0) total_ms_multi = 0;
            time_stamps.push_back(total_ms_multi);
            lyric_text = TrimWhitespace(std::string_view(lyric_text).substr(time_tag_end_index_multi + 1));
        }
        if (time_stamps.empty())
			throw std::runtime_error("Invalid lrc time tag, aborting!");
        if (lyric_text.empty()) {
            // move to next line
            start = end + 1;
            continue;
        }
        for (int time_stamp : time_stamps) 
            time_lines.push_back({ time_stamp, lyric_text });

        start = end + 1;
    }

    // stable sort lrc lines
    // 使用快排会打乱时间戳原始数据
    std::ranges::stable_sort(time_lines,
                             [](const CachedTimeLine& a, const CachedTimeLine& b)
                             {
                                 return a.time_stamp_ms < b.time_stamp_ms;
                             });
    int slash_inline_translation_count = 0;
    for (const auto& line : time_lines)
    {
        std::string original;
        std::string translation;
        if (TrySplitSlashInlineTranslation(line.text, original, translation))
            ++slash_inline_translation_count;
    }
    const bool slash_translation_enabled =
        !time_lines.empty()
        && static_cast<double>(slash_inline_translation_count) / static_cast<double>(time_lines.size())
            > InlineSlashTranslationRatioThreshold;
    std::vector<CachedTimeLine> expanded_time_lines;
    expanded_time_lines.reserve(time_lines.size() + slash_inline_translation_count);
    bool has_inline_translation = false;
    for (const auto& line : time_lines)
    {
        const auto split_texts = SplitInlineTranslationText(line.text, slash_translation_enabled);
        if (split_texts.size() > 1)
            has_inline_translation = true;

        for (const auto& split_text : split_texts)
            expanded_time_lines.push_back({ line.time_stamp_ms, split_text });
    }
    if (has_inline_translation)
        time_lines = std::move(expanded_time_lines);

    std::vector<std::vector<LrcLanguageHelper::LanguageType>> lang_types_node_seq;
    std::vector<LrcLanguageHelper::LanguageType> lang_types;
    if (time_lines.empty())
		throw std::runtime_error("Empty LRC file or no data found, aborting!");
    int time_stamp_ms_cur = time_lines[0].time_stamp_ms;
    auto& detector_instance = LrcLanguageHelper::GetSingleton();
    // 清洗控制点
    for (const CachedTimeLine& line : time_lines)
    {
        if (time_stamp_ms_cur != line.time_stamp_ms)
        {
            lang_types_node_seq.push_back(lang_types);
            lang_types.clear();
            time_stamp_ms_cur = line.time_stamp_ms;
        }
        lang_types.push_back(detector_instance.detect_line_language_type(line.text));
    }
    lang_types_node_seq.push_back(lang_types);
    lang_types.clear();
    for (const auto& multi_line_tag : lang_types_node_seq)
        for (const auto& single_line_tag : multi_line_tag)
            lang_types.push_back(single_line_tag);
    auto classification = detector_instance.detect_song_language_classification(lang_types);
    romanization_schema = RomanizationSchemaFromClassification(classification);
    NATIVE_TRACE("info: detected classification = %d\n", classification);
    auto slot_type = detector_instance.detect_language_slot(lang_types_node_seq);
    
    auto pump_stack = [&]()
    {
        std::vector<std::string> lrc_texts;
        while (!lyrics_in_ms.empty())
        {
            lrc_texts.push_back(lyrics_in_ms.top());
            lyrics_in_ms.pop();
        }
        if (lrc_texts.size() > 1)
            std::reverse(lrc_texts.begin(), lrc_texts.end());
        if (lrc_texts.empty())
            return;
        if (LrcAbstractNode* node = LrcNodeFactory::CreateLrcNode(recorded_ms, lrc_texts, classification, slot_type))
        {
            if (!lrc_nodes.empty() && !lrc_nodes.back()->is_progress_node())
            {
                lrc_nodes[lrc_nodes.size() - 1]->set_lrc_end_timestamp(recorded_ms);
            }
            lrc_nodes.push_back(node);
        }
        else
        {
            // AfxMessageBox(_T("err: create lrc node failed, aborting!"), MB_ICONERROR);
			throw std::runtime_error("Create lrc node failed, aborting!");
        }
    };

    for (size_t i = 0; i < time_lines.size(); ++i)
    {
        int total_ms = time_lines[i].time_stamp_ms;

        if (total_ms != recorded_ms)
        {
            // 新的时间戳，先处理之前的歌词
            if (!lyrics_in_ms.empty())
                pump_stack();
            recorded_ms = total_ms;
        }
        lyrics_in_ms.push(time_lines[i].text);
    }
    // 处理最后一组
    if (!lyrics_in_ms.empty())
        pump_stack();

    if (!lrc_nodes.empty()
        && !lrc_nodes.back()->is_progress_node()
        && song_end_time_ms > lrc_nodes.back()->get_time_ms())
    {
        lrc_nodes.back()->set_lrc_end_timestamp(song_end_time_ms);
    }
}

void LrcFileController::clear_lrc_nodes()
{
    for (size_t i = 0; i < lrc_nodes.size(); i++)
    {
        delete lrc_nodes[i];
    }
    lrc_nodes.clear();
}

std::string LrcFileController::to_intermediate_json(bool pretty) const
{
    rapidjson::StringBuffer buffer;
    auto write_json = [&](auto& writer)
    {
        writer.StartObject();
        writer.Key("format_version");
        writer.Int(2);
        if (!romanization_schema.empty())
        {
            writer.Key("romanization_schema");
            writer.String(romanization_schema.c_str());
        }
        writer.Key("offset");
        writer.Int(lrc_offset_ms);
        writer.Key("metadata");
        writer.StartObject();
        WriteMetadataField(writer, "artist", metadata.artist);
        WriteMetadataField(writer, "album", metadata.album);
        WriteMetadataField(writer, "author", metadata.author);
        WriteMetadataField(writer, "by", metadata.by);
        WriteMetadataField(writer, "title", metadata.title);
        writer.EndObject();
        writer.Key("lyric_lines");
        writer.StartArray();
        for (int i = 0; i < static_cast<int>(lrc_nodes.size()); ++i)
        {
            const auto* node = lrc_nodes[i];
            const int start_ms = node->get_time_ms();
            const int intrinsic_end_ms = node->get_intrinsic_end_time_ms();
            int end_ms = start_ms;
            if (i + 1 < static_cast<int>(lrc_nodes.size()))
            {
                const int next_start_ms = lrc_nodes[i + 1]->get_time_ms();
                end_ms = next_start_ms;
                if (node->is_progress_node()
                    && intrinsic_end_ms > start_ms
                    && intrinsic_end_ms < next_start_ms)
                {
                    end_ms = intrinsic_end_ms;
                }
            }
            else
            {
                end_ms = std::max(start_ms, intrinsic_end_ms);
            }
            if (end_ms <= start_ms)
            {
                end_ms = start_ms + 1;
            }

            writer.StartObject();
            writer.Key("time_start_ms");
            writer.Int(start_ms);
            writer.Key("time_end_ms");
            writer.Int(end_ms);
            writer.Key("lines");
            writer.StartArray();

            const int line_count = node->get_lrc_str_count();
            for (int line_index = 0; line_index < line_count; ++line_index)
            {
                std::string text;
                node->get_lrc_str_at(line_index, text);
                const auto aux_info = node->get_auxiliary_info(line_index);
                const auto language = node->get_language_type(line_index);
                const auto language_str = LanguageTypeToString(language);
                const int controller_node_count = node->get_controller_node_count(line_index);

                writer.StartObject();
                writer.Key("role");
                writer.String(AuxInfoToRole(aux_info));
                if (controller_node_count > 0)
                {
                    writer.Key("sync");
                    writer.String("controller_nodes");
                    writer.Key("language");
                    writer.String(language_str);
                    writer.Key("controller_nodes");
                    writer.StartArray();
                    for (int node_index = 0; node_index < controller_node_count; ++node_index)
                    {
                        int controller_start_ms = 0;
                        int controller_end_ms = 0;
                        std::string controller_text;
                        node->get_controller_node_at(
                            line_index,
                            node_index,
                            controller_start_ms,
                            controller_end_ms,
                            controller_text);

                        writer.StartObject();
                        writer.Key("time_start_ms");
                        writer.Int(controller_start_ms);
                        writer.Key("time_end_ms");
                        writer.Int(controller_end_ms);
                        writer.Key("text");
                        WriteUtf8String(writer, controller_text);
                        writer.EndObject();
                    }

                    writer.EndArray();
                }
                else
                {
                    writer.Key("language");
                    writer.String(language_str);
                    writer.Key("text");
                    WriteUtf8String(writer, text);
                }

                writer.EndObject();
            }

            writer.EndArray();
            writer.EndObject();
        }

        writer.EndArray();
        writer.EndObject();
    };

    if (pretty)
    {
        rapidjson::PrettyWriter writer(buffer);
        write_json(writer);
    }
    else
    {
        rapidjson::Writer writer(buffer);
        write_json(writer);
    }

    return { buffer.GetString(), buffer.GetSize() };
}

LrcMetadataTypeNative LrcFileController::get_metadata_type(const std::string& str)
{
    if (str.empty() || str.size() < 3 || str[0] != '[')
    {
        return LrcMetadataTypeNative::Error;
    }
    // 逐字歌词有可能每个单位后都带有时间戳
    if (str.find(']') != str.size() - 1)
        return LrcMetadataTypeNative::Error;
    const size_t metadata_end_index = str.find(':', 1);
    if (metadata_end_index == std::string::npos)
        return LrcMetadataTypeNative::Error;

    std::string metadata_type_str = str.substr(1, metadata_end_index - 1);
    std::ranges::for_each(metadata_type_str, [](const char i)
    {
       return std::isalpha(i) ? std::tolower(i) : i;
    });
    if (metadata_type_str == "ar" || metadata_type_str == "artist")
        return LrcMetadataTypeNative::Artist;
    if (metadata_type_str == "al" || metadata_type_str == "album")
        return LrcMetadataTypeNative::Album;
    if (metadata_type_str == "ti" || metadata_type_str == "title")
        return LrcMetadataTypeNative::Title;
    if (metadata_type_str == "by")
        return LrcMetadataTypeNative::By;
    if (metadata_type_str == "offset")
        return LrcMetadataTypeNative::Offset;
    if (metadata_type_str == "au" || metadata_type_str == "author")
        return LrcMetadataTypeNative::Author;
    return LrcMetadataTypeNative::Ignored;
}

std::string LrcFileController::get_metadata_value(const std::string& str)
{
    const size_t metadata_end_index = str.find(':', 1);
    if (metadata_end_index == std::string::npos)
        return {};

    return TrimWhitespace(TrimChar(std::string_view(str).substr(metadata_end_index + 1), ']'));
}

