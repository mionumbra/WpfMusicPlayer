#include "pch.h"
#include "LrcFileController.h"
#include "LocaleConverter.h"
#include <cwchar>
#include <cwctype>
#include <regex>
#include <ranges>
#include <string_view>

#include <msclr/marshal_cppstd.h>
#include <vcclr.h>

using namespace MusicPlayerLibrary;

namespace
{
std::wstring TrimWhitespace(std::wstring_view value)
{
    size_t first = 0;
    while (first < value.size() && std::iswspace(value[first]) != 0)
        ++first;

    size_t last = value.size();
    while (last > first && std::iswspace(value[last - 1]) != 0)
        --last;

    return std::wstring(value.substr(first, last - first));
}

std::wstring TrimChar(std::wstring_view value, wchar_t ch)
{
    size_t first = 0;
    while (first < value.size() && value[first] == ch)
        ++first;

    size_t last = value.size();
    while (last > first && value[last - 1] == ch)
        --last;

    return std::wstring(value.substr(first, last - first));
}
}

template <typename T>
static int FindVectorIndex(const std::vector<T>& values, const T& value)
{
    auto it = std::ranges::find(values, value);
    return it == values.end() ? -1 : static_cast<int>(std::distance(values.begin(), it));
}

// 没用了。至少证明我在规则匹配上努力过。但是一直打补丁永远不是解决问题的方法。
// IsRomajiSyllableToken and IsStrongSeparatedRomaji removed.
static std::regex time_tag_regex(R"(\[\s*(\d{1,2})\s*[:.]\s*(\d{1,2})(?:\s*[:.]\s*(\d{1,4}))?\s*\])");
static std::regex time_tag_progress_regex(R"(\s*(\d{1,2})\s*[:.]\s*(\d{1,2})(?:\s*[:.]\s*(\d{1,4}))?\s*)");

std::wstring SplitLrcForProgressMultiNode1(const std::wstring& text) 
{
    std::wstring new_text;
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

std::vector<std::wstring> SplitLrcForProgressMultiNode2(const std::vector<std::wstring>& texts)
{
    std::vector<std::wstring> strs;
    strs.reserve(texts.size());
    for (const auto& text : texts)
    {
        strs.push_back(SplitLrcForProgressMultiNode1(text));
    }
    return strs;
}

LrcMultiNode::LrcMultiNode(int t, const std::vector<std::wstring>& texts, 
    LrcLanguageHelper::LanguageClassification classification,
    std::vector<LrcLanguageHelper::LanguageType> recommend_slot) :
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
                    assign_with_language(jyut_index, LrcAuxiliaryInfoNative::Lyric);
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
    dlib::deserialize("lyric_lang_mlp.dat") >> line_net_reasoning >> line_vocab_reasoning;
    dlib::deserialize("song_structure_mlp.dat") >> song_net_reasoning;
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

std::vector<double> LrcLanguageHelper::extract_line_features(const std::wstring& text_utf16,
                                                             const std::unordered_map<std::string, int>& vocab)
{
    std::string text = LocaleConverterNative::GetUtf8StringFromUtf16String(text_utf16);
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

LrcLanguageHelper::LanguageType
LrcLanguageHelper::detect_line_language_type(const std::wstring& input)
{
    auto feat = extract_line_features(input, line_vocab_reasoning);
    line_sample_type m(feat.size());
    for (size_t i = 0; i < feat.size(); ++i)
        m(i) = feat[i];
    std::lock_guard lock(dlib_mutex);
    switch (int label_id = line_net_reasoning(m); label_id)
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
    feat = 0;

    // 语言计数
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

    // 语言计数（8维）
    for (int i = 0; i < LANGS; i++)
        feat(idx++) = count[i];

    // 语言比例（8维）
    double total = seq.size();
    for (int i = 0; i < LANGS; i++)
        feat(idx++) = count[i] / total;

    // bigram矩阵（64维）
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
            feat(idx++) = trans[i][j];

    // 语言切换次数（1维）
    int switches = 0;
    for (size_t i = 1; i < seq.size(); i++)
        if (seq[i] != seq[i - 1])
            switches++;
    feat(idx++) = switches;

    // 是否包含翻译（1维）
    feat(idx++) = count[0] > 0 && (count[1] > 0 || count[2] > 0 || count[3] > 0);

    // 是否包含罗马音（1维）
    feat(idx++) = count[5] > 0;

    // 是否包含粤拼（1维）
    feat(idx) = count[4] > 0;

    return feat;
}

LrcLanguageHelper::LanguageClassification LrcLanguageHelper::detect_song_language_classification(
    const std::vector<LrcLanguageHelper::LanguageType>& lyric_lang_type)
{
    auto song_feat = extract_song_features(lyric_lang_type);
    int reasoning_result;
    {
        std::lock_guard lock(dlib_mutex);
        reasoning_result = song_net_reasoning(song_feat);
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
    ATLTRACE("info: detect slot type = %s", best_slot_debug_str.c_str());
    return best_slot_type;
}

LrcLanguageHelper& LrcLanguageHelper::GetSingleton()
{
    static LrcLanguageHelper helper_instance;
    return helper_instance;
}

LrcProgressNode::LrcProgressNode(int t, const std::wstring& text_with_node)
    : LrcAbstractNode(t), node_count(0), end_time_ms(0)
{
    std::wstring text = text_with_node;
    const wchar_t right_brace_type = text[text.size() - 1];
    wchar_t left_brace_type;
    switch (right_brace_type)
    {
        case L']': left_brace_type = L'['; break;
        case L'>': left_brace_type = L'<'; break;
        default: return;
    }
    while (true)
    {
        const size_t right_brace_index = text.find(right_brace_type);
        if (right_brace_index == std::wstring::npos)
            break;

        std::wstring node = text.substr(0, right_brace_index + 1);
        const size_t node_controller_start_index = node.find(left_brace_type);
        if (node_controller_start_index == std::wstring::npos)
        {
            ATLTRACE(L"warn: invalid progress node: %s\n", node.c_str());
            break;
        }
        auto time_stamp = node.substr(node_controller_start_index + 1);
        std::erase(time_stamp, right_brace_type);
        auto lyric_text = node.substr(0, node_controller_start_index);
        auto time_stamp_std_string = LocaleConverterNative::GetUtf8StringFromUtf16String(time_stamp);
        std::smatch m;
        if (std::regex_search(time_stamp_std_string, m, time_tag_progress_regex)
            && m.size() == 4)
        {
            int minutes = std::stoi(m[1].str());
            int seconds = std::stoi(m[2].str());
            std::string milliseconds_str = m[3].str();
            int milliseconds = std::stoi(milliseconds_str);
            if (milliseconds_str.size() < 3)
            {
                auto multiples = 3 - milliseconds_str.size();
                milliseconds *= std::floor(pow(10, multiples));
            }
            if (milliseconds_str.size() >= 4)
            {
                auto multiples = milliseconds_str.size() - 3;
                milliseconds /= std::floor(pow(10, multiples));
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

float LrcProgressNode::get_lrc_percentage(float current_timestamp) const
{
    auto base = nodes.size();
    if (base == 0) return 1.0f;

    const int timestamp_in_ms = static_cast<int>(current_timestamp * 1000);
    if (timestamp_in_ms >= nodes[base - 1].time_ms)
    {
        return 1.0f;
    }
    if (timestamp_in_ms < time_ms)
    {
        return 0.0f;
    }

    int index;
    float percentage_in_node = 0.0f, distance = 0.0f, percentage = 0.0f;
    for (index = 0; index < base; ++index)
    {
        if (timestamp_in_ms < nodes[index].time_ms) break;
    }

    if (index != 0)
    {
        distance = static_cast<float>(nodes[index].time_ms - nodes[index - 1].time_ms);
        percentage_in_node = static_cast<float>(timestamp_in_ms - nodes[index - 1].time_ms) / distance;
    }
    else
    {
        // time_ms->start
        distance = static_cast<float>(nodes[0].time_ms - time_ms);
        if (distance > 0)
            percentage_in_node = static_cast<float>(timestamp_in_ms - time_ms) / distance;
        else
            percentage_in_node = 1.0f;
    }

    percentage = static_cast<float>(index) / base + (percentage_in_node / base);
    return percentage;
}

int SelectBestControllerLine(const std::vector<std::wstring>& str_arr)
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
    int max_index = std::distance(controller_bucket.begin(), max_it);
    return max_index;
}

LrcProgressMultiNode::LrcProgressMultiNode
    (int t, const std::vector<std::wstring>& str_arr_2, 
        LrcLanguageHelper::LanguageClassification classification, 
        std::vector<LrcLanguageHelper::LanguageType> recommend_slot):
    LrcAbstractNode(t),
    LrcMultiNode(t, SplitLrcForProgressMultiNode2(str_arr_2), classification, recommend_slot),
    LrcProgressNode(t, str_arr_2[SelectBestControllerLine(str_arr_2)]) { }

LrcFileControllerNative::~LrcFileControllerNative()
{
    clear_lrc_nodes();
}

void LrcFileControllerNative::parse_lrc_file(const std::wstring& file_path)
{
    clear_lrc_nodes();
	if (file_path.empty()
		|| file_path.find(L".lrc") == std::wstring::npos
		|| !GetDefaultFileSystem().FileExists(file_path))
		return;
	auto file = GetDefaultFileSystem().OpenReadFile(file_path, false, true);
	if (!file)
	{
		return;
	}
	parse_lrc_file_stream(file.get());
}

LrcAbstractNode* LrcNodeFactory::CreateLrcNode(
    int time_ms, const std::vector<std::wstring>& lrc_texts, 
    LrcLanguageHelper::LanguageClassification classification,
    std::vector<LrcLanguageHelper::LanguageType> recommend_slot)
{
    auto ifLrcContainsControllerNode = [](const std::wstring& lrc_text)
        {
            const auto last_index = lrc_text.size() - 1;
            return !lrc_text.empty() &&
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

void LrcFileControllerNative::parse_lrc_file_stream(IFile* file_stream)
{
    // 当前支持的格式：
    // 逐行LRC，逐字LRC，Extended LRC，交错翻译，同步翻译
    if (file_stream == nullptr)
    {
        return;
    }
    clear_lrc_nodes();
    std::string file_content_a;
    const int buf_size = 4096;
    char buffer[buf_size];
    UINT bytes_read = 0;
    do
    {
        bytes_read = file_stream->Read(buffer, buf_size - 1);
        buffer[bytes_read] = '\0';
        file_content_a.append(buffer, bytes_read);
    }
    while (bytes_read > 0);

    // 转换为宽字符
    std::wstring file_content_w = LocaleConverterNative::GetUtf16StringFromUtf8String(file_content_a);
    
    // fix issue #12
    // 关于歌词文件/歌曲内嵌歌词内出现时间tag非强制有序的翻译歌词时程序的错误/闪退问题
    // struct definition: caching each line for strong_ordering sort
    struct CachedTimeLine
    {
        int time_stamp_ms;
        std::wstring text;
    };
    std::vector<CachedTimeLine> time_lines;
    
    // 逐行解析
    size_t start = 0;
    int flag_decoding_metadata = 1;
    std::stack<std::wstring> lyrics_in_ms;
    int recorded_ms = 0;

    while (start < file_content_w.size())
    {
        size_t end = file_content_w.find(L'\n', start);
        if (end == std::wstring::npos)
        {
            end = file_content_w.size();
            // 因为现在缓存所有歌词行，所以不需要设置is_lrc_end flag
        }
        std::wstring line = TrimWhitespace(std::wstring_view(file_content_w).substr(start, end - start));
        if (line.empty())
        {
            start = end + 1;
            continue;
        }
        if (line[0] == '{')
        {
            ATLTRACE("warn: invalid ncm extension found, ignoring\n");
            start = end + 1;
            continue;
        }
        const size_t line_start_index = line.find(L'[');
        // 剔除行开头的不合法字符
        if (line_start_index != std::wstring::npos && line_start_index != 0)
        {
            ATLTRACE(L"warn: invalid lrc format, ignoring start character: %s\n",
                line.substr(0, line_start_index).c_str());
            line.erase(0, line_start_index);
        }

        // fix: moving decode_metadata as a lambda
        auto decode_metadata = [](const std::wstring& line, decltype(metadata)& meta, int& offset) -> int {
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
                offset = static_cast<int>(std::wcstol(get_metadata_value(line).c_str(), nullptr, 10));
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
        if (line.size() < 10)
        {
            clear_lrc_nodes();
            throw gcnew System::InvalidOperationException("Invalid lrc line, aborting!");
        }

        std::wstring lyric_text = line;
        // 处理同一行多个时间戳的问题
        std::vector<int> time_stamps;
        while (!lyric_text.empty() && lyric_text[0] == '[')
        {
            const size_t time_tag_end_index_multi = lyric_text.find(L']');
            if (time_tag_end_index_multi == std::wstring::npos)
                throw gcnew System::InvalidOperationException("Invalid lrc time tag, aborting!");
            auto time_tag = LocaleConverterNative::GetUtf8StringFromUtf16String(
                lyric_text.substr(0, time_tag_end_index_multi + 1));
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
                lyric_text = TrimWhitespace(std::wstring_view(lyric_text).substr(time_tag_end_index_multi + 1));
                continue;
            }
            int minutes = std::stoi(m[1].str());
            int seconds = std::stoi(m[2].str());
            std::string milliseconds_str = m[3].str();
            int milliseconds = std::stoi(milliseconds_str);
            if (milliseconds_str.size() < 3)
            {
                auto multiples = 3 - milliseconds_str.size();
                milliseconds *= std::floor(pow(10, multiples));
            }
            if (milliseconds_str.size() >= 4)
            {
                auto multiples = milliseconds_str.size() - 3;
                milliseconds /= std::floor(pow(10, multiples));
            }
            int total_ms_multi = minutes * 60000 + seconds * 1000 + milliseconds;
            if (total_ms_multi < 0) total_ms_multi = 0;
            time_stamps.push_back(total_ms_multi);
            lyric_text = TrimWhitespace(std::wstring_view(lyric_text).substr(time_tag_end_index_multi + 1));
        }
        if (time_stamps.empty())
            throw gcnew System::InvalidOperationException("Invalid lrc time tag, aborting!");
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
    std::vector<std::vector<LrcLanguageHelper::LanguageType>> lang_types_node_seq;
    std::vector<LrcLanguageHelper::LanguageType> lang_types;
    if (time_lines.empty())
        throw gcnew System::InvalidOperationException("Empty LRC file or no data found, aborting!");
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
    ATLTRACE("info: detected classification = %d\n", classification);
    auto slot_type = detector_instance.detect_language_slot(lang_types_node_seq);
    
    auto pump_stack = [&](bool is_lrc_ended)
    {
        std::vector<std::wstring> lrc_texts;
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
            if (!lrc_nodes.empty())
            {
                lrc_nodes[lrc_nodes.size() - 1]->set_lrc_end_timestamp(recorded_ms);
            }
            if (is_lrc_ended)
            {
                node->set_lrc_end_timestamp(std::floor(this->song_duration_sec * 1000));
            }
            lrc_nodes.push_back(node);
            if (node->is_translation_enabled())
                this->set_auxiliary_info_enabled(LrcAuxiliaryInfoNative::Translation);
            if (node->is_romanization_enabled())
                this->set_auxiliary_info_enabled(LrcAuxiliaryInfoNative::Romanization);
        }
        else
        {
            // AfxMessageBox(_T("err: create lrc node failed, aborting!"), MB_ICONERROR);
            throw gcnew System::InvalidOperationException("Create lrc node failed, aborting!");
        }
    };

    for (size_t i = 0; i < time_lines.size(); ++i)
    {
        int total_ms = time_lines[i].time_stamp_ms;

        if (total_ms != recorded_ms)
        {
            // 新的时间戳，先处理之前的歌词
            if (!lyrics_in_ms.empty())
                pump_stack(false);
            recorded_ms = total_ms;
        }
        lyrics_in_ms.push(time_lines[i].text);
    }
    // 处理最后一组
    if (!lyrics_in_ms.empty())
        pump_stack(true);

    cur_lrc_node_index = 0;
}

void LrcFileControllerNative::clear_lrc_nodes()
{
    for (size_t i = 0; i < lrc_nodes.size(); i++)
    {
        delete lrc_nodes[i];
    }
    lrc_nodes.clear();
}

void LrcFileControllerNative::set_time_stamp(int time_stamp_ms_in)
{
    time_stamp_ms_in -= lrc_offset_ms;
    if (time_stamp_ms_in < time_stamp_ms)
    {
        // reverse query, set index to zero
        cur_lrc_node_index = 0;
    }
    bool found = false;
    for (size_t i = cur_lrc_node_index; i < lrc_nodes.size(); i++)
    {
        if (lrc_nodes[i]->get_time_ms() > time_stamp_ms_in)
        {
            cur_lrc_node_index = i == 0 ? 0 : i - 1;
            found = true;
            break;
        }
    }
    if (!found)
    {
        cur_lrc_node_index = lrc_nodes.size() - 1;
    }
    time_stamp_ms = time_stamp_ms_in + lrc_offset_ms;
}

void LrcFileControllerNative::time_stamp_increase(int ms)
{
    time_stamp_ms += ms;
    set_time_stamp(time_stamp_ms);
}

bool LrcFileControllerNative::valid() const
{
    return !lrc_nodes.empty() && song_duration_sec >= 0;
}

int LrcFileControllerNative::get_current_lrc_lines_count() const
{
    return lrc_nodes[cur_lrc_node_index]->get_lrc_str_count();
}

int LrcFileControllerNative::get_current_lrc_line_at(int index, std::wstring& out_str) const
{
    return lrc_nodes[cur_lrc_node_index]->get_lrc_str_at(index, out_str);
}

int LrcFileControllerNative::get_lrc_line_at(int lrc_node_index, int index, std::wstring& out_str) const
{
    if (lrc_node_index < 0 || static_cast<size_t>(lrc_node_index) >= lrc_nodes.size())
        throw gcnew System::ArgumentOutOfRangeException("lrc_node_index out of range");
    return lrc_nodes[lrc_node_index]->get_lrc_str_at(index, out_str);
}

int LrcFileControllerNative::get_current_lrc_line_aux_index(LrcAuxiliaryInfoNative info) const
{
    return lrc_nodes[cur_lrc_node_index]->get_auxiliary_info_at(info);
}

int LrcFileControllerNative::get_lrc_line_aux_index(int lrc_node_index, LrcAuxiliaryInfoNative info) const
{
    if(lrc_node_index < 0 || static_cast<size_t>(lrc_node_index) >= lrc_nodes.size())
        throw gcnew System::ArgumentOutOfRangeException("lrc_node_index out of range");
    return lrc_nodes[lrc_node_index]->get_auxiliary_info_at(info);
}

int MusicPlayerLibrary::LrcFileControllerNative::get_metadata_info(LrcMetadataTypeNative metadata_type, std::wstring& out_str) const
{
    switch (metadata_type) {
    case LrcMetadataTypeNative::Album:
        out_str = metadata.album;
        break;
    case LrcMetadataTypeNative::Artist:
        out_str = metadata.artist;
        break;
    case LrcMetadataTypeNative::Title:
        out_str = metadata.title;
        break;
    case LrcMetadataTypeNative::By:
        out_str = metadata.by;
        break;
    case LrcMetadataTypeNative::Author:
        out_str = metadata.author;
        break;
    default:
        return -1;
    }
    return 0;
}

LrcLanguageInfo MusicPlayerLibrary::LrcFileControllerNative::scan_lrc_main_language_type()
{
    return LrcLanguageInfo();
}

void MusicPlayerLibrary::LrcFileControllerNative::correct_lrc_language_info(LrcLanguageInfo info)
{

}

LrcMetadataTypeNative LrcFileControllerNative::get_metadata_type(const std::wstring& str)
{
    if (str.empty() || str.size() < 3 || str[0] != '[')
    {
        return LrcMetadataTypeNative::Error;
    }
    // 逐字歌词有可能每个单位后都带有时间戳
    if (str.find(L']') != str.size() - 1)
        return LrcMetadataTypeNative::Error;
    const size_t metadata_end_index = str.find(L':', 1);
    if (metadata_end_index == std::wstring::npos)
        return LrcMetadataTypeNative::Error;

    switch (std::wstring metadata_type_str = str.substr(1, metadata_end_index - 1);
        wide_string_hash_fnv_64bit_int(metadata_type_str))
    {
    case 0x645d220c: return LrcMetadataTypeNative::Artist;
    case 0x63d58dce: return LrcMetadataTypeNative::Album;
    case 0x0387b4f0: return LrcMetadataTypeNative::Title;
    case 0x27a9be4e: return LrcMetadataTypeNative::By;
    case 0x4f6518ce: return LrcMetadataTypeNative::Offset;
    case 0x642cb63f: return LrcMetadataTypeNative::Author;
    default: return LrcMetadataTypeNative::Ignored;
    }
}

int LrcFileControllerNative::wide_string_hash_fnv_64bit_int(const std::wstring& str)
{
    const wchar_t* p = str.data();
    const size_t len = str.size();
    unsigned long long h = 14695981039346656037ull; // fnv offset basis
    const unsigned char* bytes = reinterpret_cast<const unsigned char*>(p); // NOLINT(*-use-auto)
    const size_t count = len * sizeof(wchar_t);
    for (size_t i = 0; i < count; ++i)
    {
        h ^= bytes[i];
        h *= 1099511628211ull; // fnv prime
    }
    return static_cast<int>(h % 0x7fffffffull); // switch-case requires int
}

std::wstring LrcFileControllerNative::get_metadata_value(const std::wstring& str)
{
    const size_t metadata_end_index = str.find(L':', 1);
    if (metadata_end_index == std::wstring::npos)
        return {};

    return TrimWhitespace(TrimChar(std::wstring_view(str).substr(metadata_end_index + 1), L']'));
}

// ============================================================
// Managed wrapper: LrcFileController
// ============================================================

static LrcAuxiliaryInfoNative ToNativeAuxInfo(LrcAuxiliaryInfo info)
{
    switch (info)
    {
    case LrcAuxiliaryInfo::Lyric:          return LrcAuxiliaryInfoNative::Lyric;
    case LrcAuxiliaryInfo::Translation:    return LrcAuxiliaryInfoNative::Translation;
    case LrcAuxiliaryInfo::Romanization:   return LrcAuxiliaryInfoNative::Romanization;
    case LrcAuxiliaryInfo::Ignored:        return LrcAuxiliaryInfoNative::Ignored;
    default:                               return LrcAuxiliaryInfoNative::Ignored;
    }
}

static LrcMetadataTypeNative ToNativeMetadataType(LrcMetadataType type)
{
    switch (type)
    {
    case LrcMetadataType::Title:     return LrcMetadataTypeNative::Title;
    case LrcMetadataType::Ignored:   return LrcMetadataTypeNative::Ignored;
    case LrcMetadataType::Artist:    return LrcMetadataTypeNative::Artist;
    case LrcMetadataType::Album:     return LrcMetadataTypeNative::Album;
    case LrcMetadataType::Author:    return LrcMetadataTypeNative::Author;
    case LrcMetadataType::By:        return LrcMetadataTypeNative::By;
    case LrcMetadataType::Offset:    return LrcMetadataTypeNative::Offset;
    default:                         return LrcMetadataTypeNative::Error;
    }
}

LrcFileController::LrcFileController()
{
    native_handle = new LrcFileControllerNative();
}

void LrcFileController::check_if_null()
{
    if (!native_handle)
        throw gcnew System::InvalidOperationException("LrcFileControllerNative is not initialized!");
}

void LrcFileController::ParseLrcFile(System::String^ filePath)
{
    check_if_null();
    pin_ptr<const wchar_t> wch = PtrToStringChars(filePath);
    native_handle->parse_lrc_file(std::wstring(wch));
}

void LrcFileController::ParseLrcStream(System::String^ lrcString)
{
    check_if_null();
    pin_ptr<const wchar_t> wch = PtrToStringChars(lrcString);
    const std::string utf8Str = LocaleConverterNative::GetUtf8StringFromUtf16String(std::wstring(wch));
	auto mem_file = GetDefaultFileSystem().CreateMemoryFile();
	if (!mem_file)
		return;
	mem_file->Write(utf8Str.data(), static_cast<UINT>(utf8Str.size()));
	mem_file->SeekToBegin();
	native_handle->parse_lrc_file_stream(mem_file.get());
}

void LrcFileController::ClearLrcNodes()
{
    check_if_null();
    native_handle->clear_lrc_nodes();
}

void LrcFileController::SetTimeStamp(int timeStampMs)
{
    check_if_null();
    native_handle->set_time_stamp(timeStampMs);
}

void LrcFileController::TimeStampIncrease(int ms)
{
    check_if_null();
    native_handle->time_stamp_increase(ms);
}

void LrcFileController::SetSongDuration(float durationSec)
{
    check_if_null();
    native_handle->set_song_duration(durationSec);
}

int LrcFileController::GetLrcOffset()
{
    check_if_null();
    return native_handle->get_lrc_offset();
}

void LrcFileController::SetLrcOffsetExt(int offsetMs)
{
    check_if_null();
    native_handle->set_lrc_offset_ext(offsetMs);
}

bool LrcFileController::Valid()
{
    check_if_null();
    return native_handle->valid();
}

int LrcFileController::GetCurrentTimeStamp()
{
    check_if_null();
    return native_handle->get_current_time_stamp();
}

int LrcFileController::GetCurrentLrcLinesCount()
{
    check_if_null();
    return native_handle->get_current_lrc_lines_count();
}

int LrcFileController::GetCurrentLrcNodeIndex()
{
    check_if_null();
    return native_handle->get_current_lrc_node_index();
}

int LrcFileController::GetLrcNodeCount()
{
    check_if_null();
    return native_handle->get_lrc_node_count();
}

int LrcFileController::GetLrcNodeTimeMs(int index)
{
    check_if_null();
    return native_handle->get_lrc_node_time_ms(index);
}

System::String^ LrcFileController::GetCurrentLrcLineAt(int index)
{
    check_if_null();
    std::wstring out_str;
    int result = native_handle->get_current_lrc_line_at(index, out_str);
    if (result != 0)
        return nullptr;
    return gcnew System::String(out_str.c_str());
}

System::String^ LrcFileController::GetLrcLineAt(int lrcNodeIndex, int index)
{
    check_if_null();
    std::wstring out_str;
    int result = native_handle->get_lrc_line_at(lrcNodeIndex, index, out_str);
    if (result != 0)
        return nullptr;
    return gcnew System::String(out_str.c_str());
}

int LrcFileController::GetCurrentLrcLineAuxIndex(LrcAuxiliaryInfo info)
{
    check_if_null();
    return native_handle->get_current_lrc_line_aux_index(ToNativeAuxInfo(info));
}

int LrcFileController::GetLrcLineAuxIndex(int lrcNodeIndex, LrcAuxiliaryInfo info)
{
    check_if_null();
    return native_handle->get_lrc_line_aux_index(lrcNodeIndex, ToNativeAuxInfo(info));
}

System::String^ MusicPlayerLibrary::LrcFileController::GetMetadataInfo(LrcMetadataType type)
{
    check_if_null();
    std::wstring out_str;
    int result = native_handle->get_metadata_info(ToNativeMetadataType(type), out_str);
    if (result != 0)
        return nullptr;
    return gcnew System::String(out_str.c_str());
}

bool LrcFileController::IsAuxiliaryInfoEnabled(LrcAuxiliaryInfo enableInfo)
{
    check_if_null();
    return native_handle->is_auxiliary_info_enabled(ToNativeAuxInfo(enableInfo));
}

void LrcFileController::SetAuxiliaryInfoEnabled(LrcAuxiliaryInfo enableInfo)
{
    check_if_null();
    native_handle->set_auxiliary_info_enabled(ToNativeAuxInfo(enableInfo));
}

void LrcFileController::ClearAuxiliaryInfoEnabled(LrcAuxiliaryInfo enableInfo)
{
    check_if_null();
    native_handle->clear_auxiliary_info_enabled(ToNativeAuxInfo(enableInfo));
}

void LrcFileController::ResetAuxiliaryInfoEnabled()
{
    check_if_null();
    native_handle->reset_auxiliary_info_enabled();
}

bool LrcFileController::IsPercentageEnabled(int index)
{
    check_if_null();
    return native_handle->is_percentage_enabled(index);
}

float LrcFileController::GetLrcPercentage(int index)
{
    check_if_null();
    return native_handle->get_lrc_percentage(index);
}

LrcFileController::~LrcFileController()
{
    delete native_handle;
    native_handle = nullptr;
}

