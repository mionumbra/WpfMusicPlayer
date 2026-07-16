// SPDX-License-Identifier: MIT

#pragma once

#include <atomic>

#include "pch.h"
#include "Core/FileAbstractionLayer.h"
#include "Lyric/MLPipeline/VocabularyIO.h"
#include "Lyric/MLPipeline/NCNNPipeline.h"

using line_sample_type = std::vector<double>;
using song_sample_type = std::vector<double>;

constexpr int NUM_CLASSES = 8;
constexpr int NUM_TYPES = 14;

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
		zh, latin, jp, kr, ru, jyut, roma, onomatopoeia
	};
	
	enum class LanguageClassification
	{
		zh_only,
		jp_only,
		kr_only,
		ru_only,
		latin_only,
		jp_zh_trans,
		jp_roma,
		latin_zh_trans,
		ru_zh_trans,
		kr_zh_trans,
		kr_roma,
		zh_jyut,
		jp_zh_trans_roma,
		kr_zh_trans_roma
	};
	
	std::unique_ptr<MLPipeline::NcnnClassifier> line_net_reasoning;
	std::unique_ptr<MLPipeline::NcnnClassifier> song_net_reasoning;
	MLPipeline::Vocabulary line_vocab_reasoning;
	// MSTest运行在多线程上，避免并发测试导致神经网络被破坏
	std::mutex dlib_mutex;
private:
	LrcLanguageHelper();
	std::atomic<bool> accepting_inference{true};
	void release_native_resources() noexcept;
public:
	~LrcLanguageHelper();
	LrcLanguageHelper& operator=(const LrcLanguageHelper&) = delete;
	LrcLanguageHelper(const LrcLanguageHelper&) = delete;
	LrcLanguageHelper(LrcLanguageHelper&&) = delete;
	LrcLanguageHelper& operator=(LrcLanguageHelper&&) = delete;
	std::string lyric_type_to_std_string(LanguageType type);
	std::vector<double> extract_line_features(const std::string& text, const MLPipeline::Vocabulary& vocab);
	
	std::vector<float> to_float_features(const std::vector<double>& features);

	song_sample_type extract_song_features(const std::vector<LrcLanguageHelper::LanguageType>& seq);
	LanguageClassification detect_song_language_classification(const std::vector<LrcLanguageHelper::LanguageType>& lyric_lang_type);
	auto detect_language_slot(
		const std::vector<std::vector<LanguageType>>& lines) -> std::vector<LanguageType>;
	LanguageType detect_line_language_type(const std::string& input_trimmed);
	static void InitializeSingleton();
	static void ShutdownSingleton() noexcept;
	static LrcLanguageHelper& GetSingleton();
};

class LrcAbstractNode {
protected:
	int time_ms;            // time in milliseconds
	int explicit_end_time_ms = 0;
public:
	explicit LrcAbstractNode(int time) : time_ms(time) {}
	virtual ~LrcAbstractNode() = default;
	[[nodiscard]] int get_time_ms() const { return time_ms; }
	[[nodiscard]] virtual int get_lrc_str_count() const = 0;
	virtual int get_lrc_str_at(int index, std::string& out_str) const = 0;
	[[nodiscard]] virtual LrcAuxiliaryInfoNative get_auxiliary_info(int index) const = 0;
	[[nodiscard]] virtual LrcLanguageHelper::LanguageType get_language_type(int index) const;
	[[nodiscard]] virtual int get_intrinsic_end_time_ms() const { return explicit_end_time_ms > 0 ? explicit_end_time_ms : time_ms; }
	[[nodiscard]] virtual int get_controller_node_count(int line_index) const { return 0; }
	virtual int get_controller_node_at(
		int line_index,
		int node_index,
		int& start_time_ms,
		int& end_time_ms,
		std::string& out_str) const { return -1; }
	virtual void set_lrc_end_timestamp(int end_time_ms) { explicit_end_time_ms = end_time_ms; }
	[[nodiscard]] virtual bool is_progress_node() const { return false; }

	bool operator<(const LrcAbstractNode& other) const {
		return time_ms < other.time_ms;
	}
};

class LrcNode final: public LrcAbstractNode {
	std::string lrc_text;       // UTF-8 lyric text
public:
	LrcNode(int t, const std::string& text)
		: LrcAbstractNode(t), lrc_text(text) {
	}

	[[nodiscard]] int get_lrc_str_count() const override {
		return 1;
	}

	int get_lrc_str_at(int index, std::string& out_str) const override {
		if (index != 0) return -1;
		return out_str = lrc_text, 0;
	}

	[[nodiscard]] LrcAuxiliaryInfoNative get_auxiliary_info(int index) const override {
		if (index == 0)
			return LrcAuxiliaryInfoNative::Lyric;
		return LrcAuxiliaryInfoNative::Ignored;
	}

};

/*
* for display lrc with translation or romanization
*/
class LrcMultiNode : virtual public LrcAbstractNode {
	friend class LrcFileController;
	int str_count;
	std::vector<std::string> lrc_texts;
	std::vector<LrcAuxiliaryInfoNative> aux_infos;
	std::vector<LrcLanguageHelper::LanguageType> lang_types;

public:

	LrcMultiNode(int t, const std::vector<std::string>& texts,
		LrcLanguageHelper::LanguageClassification classification,
		const std::vector<LrcLanguageHelper::LanguageType>& recommend_slot);

	[[nodiscard]] int get_lrc_str_count() const override {
		return str_count;
	}

	int get_lrc_str_at(int index, std::string& out_str) const override {
		if (index < 0 || index >= str_count) return -1;
		out_str = lrc_texts[index];
		return 0;
	}

	[[nodiscard]] LrcAuxiliaryInfoNative get_auxiliary_info(int index) const override
	{
		return aux_infos[index];
	}

	[[nodiscard]] LrcLanguageHelper::LanguageType get_language_type(int index) const override;
};

class LrcProgressNode: virtual public LrcAbstractNode
{
protected:
	int node_count;
	struct node_info
	{
		int time_ms;
		std::string node_text;
	};
	int end_time_ms;
	std::vector<node_info> nodes;
	int controller_line_index;
public:
	LrcProgressNode(int t, const std::string& text_with_node, int controller_line_index = 0);
	[[nodiscard]] int get_lrc_str_count() const override { return 1; }
	int get_lrc_str_at(int index, std::string& out_str) const override
	{
		if (index != 0) return -1;
		std::string text;
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
	[[nodiscard]] int get_intrinsic_end_time_ms() const override;
	[[nodiscard]] int get_controller_node_count(int line_index) const override;
	int get_controller_node_at(
		int line_index,
		int node_index,
		int& start_time_ms,
		int& end_time_ms,
		std::string& out_str) const override;
	void set_lrc_end_timestamp(int time_ms) override { this->end_time_ms = time_ms; }
	[[nodiscard]] bool is_progress_node() const override { return true; }
};

class LrcProgressMultiNode final:
	public LrcProgressNode, public LrcMultiNode
{
public:
	LrcProgressMultiNode(int t, const std::vector<std::string>& str_arr_2, 
		LrcLanguageHelper::LanguageClassification classification,
		std::vector<LrcLanguageHelper::LanguageType> recommend_slot);
	
	[[nodiscard]] int get_lrc_str_count() const override
	{
		return LrcMultiNode::get_lrc_str_count();
	}
	int get_lrc_str_at(int index, std::string& out_str) const override
	{
		return LrcMultiNode::get_lrc_str_at(index, out_str);
	}
	[[nodiscard]] LrcAuxiliaryInfoNative get_auxiliary_info(int index) const override
	{
		return LrcMultiNode::get_auxiliary_info(index);
	}
	
	[[nodiscard]] LrcLanguageHelper::LanguageType get_language_type(int index) const override { return LrcMultiNode::get_language_type(index); }
	[[nodiscard]] int get_intrinsic_end_time_ms() const override { return LrcProgressNode::get_intrinsic_end_time_ms(); }
	[[nodiscard]] int get_controller_node_count(int line_index) const override { return LrcProgressNode::get_controller_node_count(line_index); }
	[[nodiscard]] int get_controller_node_at(int line_index, int node_index, int& start_time_ms, int& end_time_ms, std::string& out_str) const override
	{
		return LrcProgressNode::get_controller_node_at(line_index, node_index, start_time_ms, end_time_ms, out_str);
	}
	[[nodiscard]] bool is_progress_node() const override { return true; }
	void set_lrc_end_timestamp(int time_ms) override { LrcProgressNode::set_lrc_end_timestamp(time_ms); }
};

class LrcNodeFactory {
public:
	static LrcAbstractNode* CreateLrcNode(int time_ms, const std::vector<std::string>& lrc_texts, LrcLanguageHelper::LanguageClassification classification, std::vector<LrcLanguageHelper::LanguageType> recommend_slot);
};

/*
* internal helper of CLrcManagerWnd, perform lyric management
*/
class LrcFileController {
	std::vector<LrcAbstractNode*> lrc_nodes;
	int lrc_offset_ms = 0;
	int song_end_time_ms = 0;
	std::string romanization_schema{};
	struct
	{
		std::string artist, album, author, by, title;
	} metadata;
public:
	explicit LrcFileController(int song_end_time_ms = 0);
	~LrcFileController();
	void parse_lrc_file_stream(IFile* file_stream);
	void clear_lrc_nodes();
	[[nodiscard]] std::string to_intermediate_json(bool pretty = false) const;

	// static helpers
	static LrcMetadataTypeNative get_metadata_type(const std::string& str);
	static std::string get_metadata_value(const std::string& str);
};

	
}
