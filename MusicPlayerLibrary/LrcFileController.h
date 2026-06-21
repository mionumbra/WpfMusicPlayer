#pragma once

#include "pch.h"
#include "FileAbstractionLayer.h"

#include <dlib/svm_threaded.h>
#include <dlib/dnn.h>

using line_sample_type = dlib::matrix<double, 0, 1>;
using song_sample_type = dlib::matrix<double>;

constexpr int NUM_CLASSES = 7;
constexpr int NUM_TYPES = 12;

using line_net_type = dlib::loss_multiclass_log<
	dlib::fc<NUM_CLASSES,
	dlib::relu<dlib::fc<64,
	dlib::relu<dlib::fc<128,
	dlib::input<line_sample_type>
	>>>>>>;

using song_net_type = dlib::loss_multiclass_log<
	dlib::fc<NUM_TYPES,
	dlib::relu<dlib::fc<32,
	dlib::relu<dlib::fc<64,
	dlib::input<song_sample_type>
	>>>>>>;

namespace MusicPlayerLibrary
{
    
enum class LrcMetadataTypeNative
{
	Artist, Album, Author, By, Offset, Title, Ignored, Error
};

enum class ThreeWayCompareResult
{
	Less = -1, Equal = 0, Greater = 1
};

enum class LrcAuxiliaryInfoNative
{
	Lyric,
	Translation,
	Romanization,
	Ignored
};

class LrcLanguageHelper
{
public:
	enum class LanguageType
	{
		zh, en, jp, kr, jyut, roma, onomatopoeia
	};
	
	enum class LanguageClassification
	{
		zh_only,
		jp_only,
		kr_only,
		en_only,
		jp_zh_trans,
		jp_roma,
		en_zh_trans,
		kr_zh_trans,
		kr_roma,
		zh_jyut,
		jp_zh_trans_roma,
		kr_zh_trans_roma
	};
	
	line_net_type line_net_reasoning;
	std::unordered_map<std::string, int> line_vocab_reasoning;
	song_net_type song_net_reasoning;
	// MSTest运行在多线程上，避免并发测试导致神经网络被破坏
	std::mutex dlib_mutex;
private:
	LrcLanguageHelper();
public:
	LrcLanguageHelper& operator=(const LrcLanguageHelper&) = delete;
	LrcLanguageHelper(const LrcLanguageHelper&) = delete;
	LrcLanguageHelper(LrcLanguageHelper&&) = delete;
	LrcLanguageHelper& operator=(LrcLanguageHelper&&) = delete;
	std::string lyric_type_to_std_string(LanguageType type);
	std::vector<double> extract_line_features(const std::wstring& text, const std::unordered_map<std::string, int>& vocab);
	song_sample_type extract_song_features(const std::vector<LrcLanguageHelper::LanguageType>& seq);
	LanguageClassification detect_song_language_classification(const std::vector<LrcLanguageHelper::LanguageType>& lyric_lang_type);
	auto detect_language_slot(
		const std::vector<std::vector<LanguageType>>& lines) -> std::vector<LanguageType>;
	LanguageType detect_line_language_type(const std::wstring& input_trimmed);
	static LrcLanguageHelper& GetSingleton();
};

class LrcAbstractNode {
protected:
	int time_ms;            // time in milliseconds
public:
	explicit LrcAbstractNode(int time) : time_ms(time) {}
	virtual ~LrcAbstractNode() = default;
	[[nodiscard]] int get_time_ms() const { return time_ms; }
	[[nodiscard]] virtual int get_lrc_str_count() const = 0;
	virtual int get_lrc_str_at(int index, std::wstring& out_str) const = 0;
	[[nodiscard]] virtual LrcAuxiliaryInfoNative get_auxiliary_info(int index) const = 0;
	[[nodiscard]] virtual int get_auxiliary_info_at(LrcAuxiliaryInfoNative info) const = 0;
	[[nodiscard]] virtual bool is_translation_enabled() const { return false; }
	[[nodiscard]] virtual bool is_romanization_enabled() const { return false; }
	[[nodiscard]] virtual float get_lrc_percentage(float current_timestamp) const = 0;
	[[nodiscard]] virtual bool is_lrc_percentage_enabled() const { return false; }
	virtual void set_lrc_end_timestamp(int end_time_ms) { }

	bool operator<(const LrcAbstractNode& other) const {
		return time_ms < other.time_ms;
	}
};

class LrcNode final: public LrcAbstractNode {
	std::wstring lrc_text;       // lyric text
public:
	LrcNode(int t, const std::wstring& text)
		: LrcAbstractNode(t), lrc_text(text) {
	}

	[[nodiscard]] int get_lrc_str_count() const override {
		return 1;
	}

	int get_lrc_str_at(int index, std::wstring& out_str) const override {
		if (index != 0) return -1;
		return out_str = lrc_text, 0;
	}

	[[nodiscard]] LrcAuxiliaryInfoNative get_auxiliary_info(int index) const override {
		if (index == 0)
			return LrcAuxiliaryInfoNative::Lyric;
		return LrcAuxiliaryInfoNative::Ignored;
	}

	[[nodiscard]] int get_auxiliary_info_at(LrcAuxiliaryInfoNative info) const override {
		if (info == LrcAuxiliaryInfoNative::Lyric) return 0;
		return -1;
	}
	[[nodiscard]] float get_lrc_percentage(float current_timestamp) const override
	{
		return 1.0f;
	}
};

/*
* for display lrc with translation or romanization
*/
class LrcMultiNode : virtual public LrcAbstractNode {
	friend class LrcFileControllerNative;
	int str_count;
	std::vector<std::wstring> lrc_texts;
	std::vector<LrcAuxiliaryInfoNative> aux_infos;
	std::vector<LrcLanguageHelper::LanguageType> lang_types;

public:

	LrcMultiNode(int t, const std::vector<std::wstring>& texts,
		LrcLanguageHelper::LanguageClassification classification,
		std::vector<LrcLanguageHelper::LanguageType> recommend_slot);

	[[nodiscard]] int get_lrc_str_count() const override {
		return str_count;
	}

	int get_lrc_str_at(int index, std::wstring& out_str) const override {
		if (index < 0 || index >= str_count) return -1;
		out_str = lrc_texts[index];
		return 0;
	}

	[[nodiscard]] LrcAuxiliaryInfoNative get_auxiliary_info(int index) const override
	{
		return aux_infos[index];
	}

	[[nodiscard]] int get_auxiliary_info_at(LrcAuxiliaryInfoNative info) const override {
		auto it = std::find(aux_infos.begin(), aux_infos.end(), info);
		return it == aux_infos.end() ? -1 : static_cast<int>(std::distance(aux_infos.begin(), it));
	}

	[[nodiscard]] bool is_translation_enabled() const override {
		return std::find(aux_infos.begin(), aux_infos.end(), LrcAuxiliaryInfoNative::Translation) != aux_infos.end();
	}

	[[nodiscard]] bool is_romanization_enabled() const override {
		return std::find(aux_infos.begin(), aux_infos.end(), LrcAuxiliaryInfoNative::Romanization) != aux_infos.end();
	}

	[[nodiscard]] float get_lrc_percentage(float current_timestamp) const override
	{
		return 1.0f;
	}
protected:
	void set_auxiliary_info_at(int index, LrcAuxiliaryInfoNative info) {
		aux_infos[index] = info;
	}
};

class LrcProgressNode: virtual public LrcAbstractNode
{
protected:
	int node_count;
	struct node_info
	{
		int time_ms;
		std::wstring node_text;
	};
	int end_time_ms;
	std::vector<node_info> nodes;
public:
	LrcProgressNode(int t, const std::wstring& text_with_node);
	[[nodiscard]] int get_lrc_str_count() const override { return 1; }
	int get_lrc_str_at(int index, std::wstring& out_str) const override
	{
		if (index != 0) return -1;
		std::wstring text;
		for (int i = 0; i < node_count; ++i)
		{
			text.append(nodes[i].node_text);
		}
		return out_str = text, 0;
	}
	[[nodiscard]] LrcAuxiliaryInfoNative get_auxiliary_info(int index) const override
	{
		if (index == 0)
			return LrcAuxiliaryInfoNative::Lyric;
		return LrcAuxiliaryInfoNative::Ignored;
	}
	[[nodiscard]] int get_auxiliary_info_at(LrcAuxiliaryInfoNative info) const override
	{
		if (info == LrcAuxiliaryInfoNative::Lyric) return 0;
		return -1;
	}
	[[nodiscard]] float get_lrc_percentage(float current_timestamp) const override;
	[[nodiscard]] bool is_lrc_percentage_enabled() const override { return true; }
	void set_lrc_end_timestamp(int time_ms) override { this->end_time_ms = time_ms; }
};

class LrcProgressMultiNode final:
	public LrcProgressNode, public LrcMultiNode
{
public:
	LrcProgressMultiNode(int t, const std::vector<std::wstring>& str_arr_2, 
		LrcLanguageHelper::LanguageClassification classification,
		std::vector<LrcLanguageHelper::LanguageType> recommend_slot);
	
	[[nodiscard]] int get_lrc_str_count() const override
	{
		return LrcMultiNode::get_lrc_str_count();
	}
	int get_lrc_str_at(int index, std::wstring& out_str) const override
	{
		return LrcMultiNode::get_lrc_str_at(index, out_str);
	}
	[[nodiscard]] LrcAuxiliaryInfoNative get_auxiliary_info(int index) const override
	{
		return LrcMultiNode::get_auxiliary_info(index);
	}
	[[nodiscard]] int get_auxiliary_info_at(LrcAuxiliaryInfoNative info) const override
	{
		return LrcMultiNode::get_auxiliary_info_at(info);
	}
	[[nodiscard]] float get_lrc_percentage(float current_timestamp) const override
	{
		return LrcProgressNode::get_lrc_percentage(current_timestamp);
	}
	[[nodiscard]] bool is_translation_enabled() const override { return LrcMultiNode::is_translation_enabled(); }
	[[nodiscard]] bool is_romanization_enabled() const override { return LrcMultiNode::is_romanization_enabled(); }
	[[nodiscard]] bool is_lrc_percentage_enabled() const override { return true; }
	void set_lrc_end_timestamp(int time_ms) override { LrcProgressNode::set_lrc_end_timestamp(time_ms); }
};

class LrcNodeFactory {
public:
	static LrcAbstractNode* CreateLrcNode(int time_ms, const std::vector<std::wstring>& lrc_texts, LrcLanguageHelper::LanguageClassification classification, std::vector<LrcLanguageHelper::LanguageType> recommend_slot);
};

struct LrcLanguageInfo {
	LrcLanguageHelper::LanguageType main_language;
	bool translation_enabled;
	bool romanization_enabled;
};

/*
* internal helper of CLrcManagerWnd, perform lyric management
*/
class LrcFileControllerNative {
	LrcLanguageHelper::LanguageClassification main_classification = LrcLanguageHelper::LanguageClassification::en_only;
	std::vector<LrcAbstractNode*> lrc_nodes;
	int time_stamp_ms = 0, lrc_offset_ms = 0;
	size_t cur_lrc_node_index = 0;
	struct
	{
		std::wstring artist, album, author, by, title;
	} metadata;
	int aux_enable_info = 0;
	float song_duration_sec = 0;
public:
	~LrcFileControllerNative();
	void parse_lrc_file(const std::wstring& file_path);
	void parse_lrc_file_stream(IFile* file_stream);
	void clear_lrc_nodes();
	void set_time_stamp(int time_stamp_ms_in);
	void time_stamp_increase(int ms);
	void set_song_duration(float duration_sec) { song_duration_sec = duration_sec; }
	void set_lrc_offset_ext(int offset_ms) { lrc_offset_ms = offset_ms; }
	[[nodiscard]] bool valid() const;
	[[nodiscard]] int get_current_time_stamp() const { return time_stamp_ms; }
	[[nodiscard]] int get_current_lrc_lines_count() const;
	[[nodiscard]] int get_current_lrc_node_index() const { return static_cast<int>(cur_lrc_node_index); }
	[[nodiscard]] int get_lrc_node_count() const { return static_cast<int>(lrc_nodes.size()); }           
	[[nodiscard]] int get_lrc_node_time_ms(int index) const { assert(index >= 0 && static_cast<size_t>(index) < lrc_nodes.size());  return lrc_nodes[index]->get_time_ms() + lrc_offset_ms; }
	int get_current_lrc_line_at(int index, std::wstring& out_str) const;
	int get_lrc_line_at(int lrc_node_index, int index, std::wstring& out_str) const;
	[[nodiscard]] int get_current_lrc_line_aux_index(LrcAuxiliaryInfoNative info) const;
	[[nodiscard]] int get_lrc_line_aux_index(int lrc_node_index, LrcAuxiliaryInfoNative info) const;
	[[nodiscard]] int get_metadata_info(LrcMetadataTypeNative metadata_type, std::wstring& out_str) const;

	[[nodiscard]] int is_auxiliary_info_enabled(LrcAuxiliaryInfoNative enable_info) const
	{
		return (aux_enable_info & (1 << static_cast<int>(enable_info))) != 0;
	}
	void set_auxiliary_info_enabled(LrcAuxiliaryInfoNative enable_info)
	{
		aux_enable_info |= (1 << static_cast<int>(enable_info));
	}
	void clear_auxiliary_info_enabled(LrcAuxiliaryInfoNative enable_info)
	{
		aux_enable_info &= ~(1 << static_cast<int>(enable_info));
	}
	void reset_auxiliary_info_enabled() { aux_enable_info = 0; }
	bool is_percentage_enabled(int index) { return lrc_nodes[index]->is_lrc_percentage_enabled(); }
	float get_lrc_percentage(int index) { return lrc_nodes[index]->get_lrc_percentage((time_stamp_ms - lrc_offset_ms) / 1000.0f); }
	int get_lrc_offset() { return lrc_offset_ms; }

	LrcLanguageInfo scan_lrc_main_language_type();
	void correct_lrc_language_info(LrcLanguageInfo info);

	// static helpers
	static LrcMetadataTypeNative get_metadata_type(const std::wstring& str);
	static int wide_string_hash_fnv_64bit_int(const std::wstring& str);
	static std::wstring get_metadata_value(const std::wstring& str);
};

public enum class LrcAuxiliaryInfo
{
	Lyric = 0,
	Translation = 1,
	Romanization = 2,
	Ignored = 3
};

public enum class LrcMetadataType
{
	Artist, Album, Author, By, Offset, Title, Ignored, Error
};

public ref class LrcFileController:
	System::IDisposable
{
	LrcFileControllerNative* native_handle;

	void check_if_null();

public:
	LrcFileController();

	void ParseLrcFile(System::String^ filePath);
	void ParseLrcStream(System::String^ lrcString);
	void ClearLrcNodes();
	void SetTimeStamp(int timeStampMs);
	void TimeStampIncrease(int ms);
	void SetSongDuration(float durationSec);
	int GetLrcOffset();
	void SetLrcOffsetExt(int offsetMs);

	bool Valid();
	int GetCurrentTimeStamp();
	int GetCurrentLrcLinesCount();
	int GetCurrentLrcNodeIndex();
	int GetLrcNodeCount();
	int GetLrcNodeTimeMs(int index);

	System::String^ GetCurrentLrcLineAt(int index);
	System::String^ GetLrcLineAt(int lrcNodeIndex, int index);

	int GetCurrentLrcLineAuxIndex(LrcAuxiliaryInfo info);
	int GetLrcLineAuxIndex(int lrcNodeIndex, LrcAuxiliaryInfo info);
	System::String^ GetMetadataInfo(LrcMetadataType type);

	bool IsAuxiliaryInfoEnabled(LrcAuxiliaryInfo enableInfo);
	void SetAuxiliaryInfoEnabled(LrcAuxiliaryInfo enableInfo);
	void ClearAuxiliaryInfoEnabled(LrcAuxiliaryInfo enableInfo);
	void ResetAuxiliaryInfoEnabled();

	bool IsPercentageEnabled(int index);
	float GetLrcPercentage(int index);

	~LrcFileController();
};
	
}
