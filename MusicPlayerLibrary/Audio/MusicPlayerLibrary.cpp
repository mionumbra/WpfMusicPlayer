// SPDX-License-Identifier: MIT

#include "pch.h"

#include "MusicPlayerLibrary.h"
#include "Audio/DSP/FapoEqualizer.h"
#include "Crypto/NcmDecryptor.h"
#include <cwctype>
#include <format>
#include <limits>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string_view>
#include "Core/LocaleConverter.h"
#include "Core/AudioThreadScheduleHelper.h"

namespace
{
	bool FAudioCallFailed(const std::uint32_t result) noexcept
	{
		return result != 0;
	}

	bool EqualsIgnoreCase(std::wstring_view left, std::wstring_view right)
	{
		if (left.size() != right.size())
			return false;

		for (size_t i = 0; i < left.size(); ++i)
		{
			if (std::towlower(left[i]) != std::towlower(right[i]))
				return false;
		}
		return true;
	}

	void ToLowerInPlace(std::wstring& value)
	{
		for (wchar_t& ch : value)
			ch = static_cast<wchar_t>(std::towlower(ch));
	}
	
	auto ToLower(const std::wstring& value) -> std::wstring
	{
		std::wstring result{value};
		ToLowerInPlace(result);
		return result;
	}

	struct AVFrameDeleter
	{
		void operator()(AVFrame* frame) const noexcept
		{
			if (frame)
				av_frame_free(&frame);
		}
	};

	using UniqueAVFrame = std::unique_ptr<AVFrame, AVFrameDeleter>;

	struct AVSamplesBuffer
	{
		uint8_t** data = nullptr;

		AVSamplesBuffer() = default;
		AVSamplesBuffer(const AVSamplesBuffer&) = delete;
		AVSamplesBuffer& operator=(const AVSamplesBuffer&) = delete;

		~AVSamplesBuffer()
		{
			reset();
		}

		int allocate(int channels, int nb_samples, AVSampleFormat sample_fmt)
		{
			reset();
			data = reinterpret_cast<uint8_t**>(av_calloc(channels, sizeof(uint8_t*)));
			if (!data)
				return AVERROR(ENOMEM);

			const int ret = av_samples_alloc(data, nullptr, channels, nb_samples, sample_fmt, 0);
			if (ret < 0)
			{
				reset();
				return ret;
			}
			return 0;
		}

		uint8_t** get() const noexcept
		{
			return data;
		}

		uint8_t* first_plane() const noexcept
		{
			return data ? data[0] : nullptr;
		}

		void reset() noexcept
		{
			if (data)
			{
				av_freep(&data[0]);
				av_free(data);
				data = nullptr;
			}
		}
	};

	void DeleteFAudioBuffer(FAudioBuffer* buffer) noexcept
	{
		if (!buffer)
			return;
		delete[] buffer->pAudioData;
		delete reinterpret_cast<int*>(buffer->pContext);
		delete buffer;
	}

	struct FAudioBufferDeleter
	{
		void operator()(FAudioBuffer* buffer) const noexcept
		{
			DeleteFAudioBuffer(buffer);
		}
	};

	using UniqueFAudioBuffer = std::unique_ptr<FAudioBuffer, FAudioBufferDeleter>;

}

int MusicPlayerLibrary::MusicPlayer::read_func(uint8_t* buf, int buf_size) {
	// ATLTRACE("info: read buf_size=%d, rest=%lld\n", buf_size, file_stream->GetLength() - file_stream->GetPosition());
	// reset file_stream_end
	file_stream_end = false;
	int gcount = static_cast<int>(file_stream->Read(buf, buf_size));
	if (gcount == 0) {
		file_stream_end = true;
		return AVERROR_EOF;
	}
	return gcount;
}

int MusicPlayerLibrary::MusicPlayer::read_func_wrapper(void* opaque, uint8_t* buf, int buf_size)
{
	auto callObject = reinterpret_cast<MusicPlayer*>(opaque);
	return callObject->read_func(buf, buf_size);
}

int64_t MusicPlayerLibrary::MusicPlayer::seek_func(int64_t offset, int whence)
{
	FileSeekOrigin origin;
	switch (whence) {
	case AVSEEK_SIZE: return static_cast<int64_t>(file_stream->GetLength());
	case SEEK_SET: origin = FileSeekOrigin::Begin; break;
	case SEEK_CUR: origin = FileSeekOrigin::Current; break;
	case SEEK_END: origin = FileSeekOrigin::End; break;
	default: return AVERROR(EINVAL); // unsupported
	}
	uint64_t pos = file_stream->Seek(offset, origin);
	return static_cast<int64_t>(pos);
}

int64_t MusicPlayerLibrary::MusicPlayer::seek_func_wrapper(void* opaque, int64_t offset, int whence)
{
	auto callObject = reinterpret_cast<MusicPlayer*>(opaque);
	return callObject->seek_func(offset, whence);
}

inline int MusicPlayerLibrary::MusicPlayer::load_audio_context(const std::wstring& audio_filename, const std::wstring& file_extension_in, bool skip_album_art_loading)
{
	// 打开文件流
	// std::ios::sync_with_stdio(false);
	file_stream = GetDefaultFileSystem().OpenReadFile(audio_filename, true, false);
	bool is_ncm = false;
	file_extension = file_extension_in;
	this->skip_album_art_loading = skip_album_art_loading;
	if (!file_stream)
	{
		NATIVE_TRACE("err: file not exists!\n");
		return -1;
	}
	char magic[10];
	if (const int ret = file_stream->Read(magic, 8); ret != 8)
	{
		NATIVE_TRACE("err: failed to read magic bytes\n");
		file_stream.reset();
		return -1;
	}
	magic[9] = '\0';
	NATIVE_TRACE("info: magic bytes: %s\n", magic);
	if (std::string_view(magic, 8) == "CTENFDAM")
	{
		NATIVE_TRACE("info: found ncm header\n");
		is_ncm = true;
	}
	file_stream->SeekToBegin();

	if (EqualsIgnoreCase(file_extension_in, L"ncm") || is_ncm)
	{
		try
		{
			file_stream->SeekToBegin();
			auto decryptor_result = NcmDecryptor::Open(std::move(file_stream), audio_filename);
			file_stream = std::move(decryptor_result.audio_file);
			file_stream->SeekToBegin();
			if (!skip_album_art_loading)
			{
				require_download_ncm_album_art(decryptor_result.metadata.pictureUrl);
			}
		}
		catch (std::exception& e)
		{
			NATIVE_TRACE("err: decrypt ncm failed: %s\n", e.what());
			NATIVE_TRACE("err: this can be caused by ncm algorithm update, or ncm file corrupt\n");
			NATIVE_TRACE("err: please try to report ncm file to issues\n");
			file_stream.reset();
			return -1;
		}
		// file_stream now exposes decrypted audio bytes without materializing an output file.
	}
	return load_audio_context_from_file_stream();
}

int MusicPlayerLibrary::MusicPlayer::load_audio_context_from_file_stream()
{
	if (!file_stream)
		return -1;

	// 重置文件流指针，防止读取后未复位
	file_stream->SeekToBegin();
	std::unique_ptr<char[]> buf(DBG_NEW char[1024]);
	memset(buf.get(), 0, sizeof(char) * 1024);
	auto fail_load = [this]() {
		uninitialize_audio_engine();
		reset_av_filter_equalizer();
		release_audio_context();
		return -1;
		};

	// 取得文件大小
	format_context = avformat_alloc_context();
	if (!format_context)
	{
		NATIVE_TRACE("err: avformat_alloc_context failed\n");
		return fail_load();
	}
	size_t file_len = static_cast<int64_t>(file_stream->GetLength());
	NATIVE_TRACE("info: file loaded, size = %zu\n", file_len);

	constexpr size_t avio_buf_size = 8192;


	buffer = reinterpret_cast<unsigned char*>(av_malloc(avio_buf_size));
	if (!buffer)
	{
		NATIVE_TRACE("err: av_malloc failed for avio buffer\n");
		return fail_load();
	}
	avio_context =
		avio_alloc_context(buffer, avio_buf_size, 0,
			this,
			&read_func_wrapper,
			nullptr,
			&seek_func_wrapper);
	if (!avio_context)
	{
		NATIVE_TRACE("err: avio_alloc_context failed\n");
		return fail_load();
	}

	format_context->pb = avio_context;

	// 打开音频文件
	int res = avformat_open_input(&format_context,
		nullptr, // dummy parameter, read from memory stream
		nullptr, // let ffmpeg auto detect format
		nullptr  // no parateter specified
	);
	if (res < 0) {
		av_strerror(res, buf.get(), 1024);
		NATIVE_TRACE("err: avformat_open_input failed: %s\n", buf.get());
		return fail_load();
	}
	if (!format_context)
	{
		av_strerror(res, buf.get(), 1024);
		NATIVE_TRACE("err: avformat_open_input failed, reason = %s(%d)\n", buf.get(), res);
		return fail_load();
	}

	res = avformat_find_stream_info(format_context, nullptr);
	if (res == AVERROR_EOF)
	{
		NATIVE_TRACE("err: no stream found in file\n");
		return fail_load();
	}
	NATIVE_TRACE("info: stream count %d\n", format_context->nb_streams);
	audio_stream_index = av_find_best_stream(format_context, AVMEDIA_TYPE_AUDIO, -1, -1, const_cast<const AVCodec**>(&codec), 0);
	if (audio_stream_index < 0) {
		NATIVE_TRACE("err: no audio stream found\n");
		return fail_load();
	}

	AVStream* current_stream = format_context->streams[audio_stream_index];
	codec = const_cast<AVCodec*>(avcodec_find_decoder(current_stream->codecpar->codec_id));
	if (!codec)
	{
		NATIVE_TRACE("warn: no valid decoder found, stream id = %d!\n", audio_stream_index);
		return fail_load();
	}

	NATIVE_TRACE("info: open stream id %d, format=%d, sample_rate=%d, channels=%d, channel_layout=%d\n",
		audio_stream_index,
		current_stream->codecpar->format,
		current_stream->codecpar->sample_rate,
		current_stream->codecpar->ch_layout.nb_channels,
		current_stream->codecpar->ch_layout.order);

	int image_stream_id = -1;

	for (unsigned int i = 0; i < format_context->nb_streams; i++) {
		if (AVStream* stream = format_context->streams[i]; stream->disposition & AV_DISPOSITION_ATTACHED_PIC) {
			NATIVE_TRACE("info: open stream id %d read attaching pic\n", i);
			image_stream_id = static_cast<int>(i);
			break;
		}
	}

	if (image_stream_id != -1 && !skip_album_art_loading && this->file_extension != L"ncm")
	{
		publish_message({PlayerMessageType::AlbumArtInitialized,
			get_id3_album_art_stream(image_stream_id)});
	}
	read_metadata();

	// 从0ms开始读取
	avformat_seek_file(format_context, -1, INT64_MIN, 0, INT64_MAX, 0);
	// codec is not null
	// 建立解码器上下文
	codec_context = avcodec_alloc_context3(codec);
	if (codec_context == nullptr)
	{
		NATIVE_TRACE("err: avcodec_alloc_context3 failed\n");
		return fail_load();
	}
	avcodec_parameters_to_context(codec_context, format_context->streams[audio_stream_index]->codecpar);

	// 降低错误容忍度
	codec_context->err_recognition = AV_EF_IGNORE_ERR | AV_EF_COMPLIANT;
	// 错误隐藏
	codec_context->error_concealment = FF_EC_GUESS_MVS | FF_EC_DEBLOCK;
	// 跳过坏帧
	codec_context->skip_frame = AVDISCARD_NONREF;

	// 解码文件
	res = avcodec_open2(codec_context, codec, nullptr);
	if (res)
	{
		av_strerror(res, buf.get(), 1024);
		NATIVE_TRACE("err: avcodec_open2 failed, reason = %s\n", buf.get());
		return fail_load();
	}

	// avoid ffmpeg warning
	codec_context->pkt_timebase = format_context->streams[audio_stream_index]->time_base;
	// set parallel decode (flac, wav..
	av_opt_set_int(codec_context, "threads", 0, 0);

	// init avaudiofifo with the same format the filter graph feeds to XAudio2.
	AVChannelLayout stereo_layout = AV_CHANNEL_LAYOUT_STEREO;
	fifo_audio_channels = stereo_layout.nb_channels;
	fifo_audio_sample_fmt = AV_SAMPLE_FMT_S16;
	fifo_sample_rate = sample_rate;
	if (!audio_fifo) {
		res = initialize_audio_fifo(fifo_audio_sample_fmt,
			fifo_audio_channels,
			1024); // initial size
		if (res < 0) {
			NATIVE_TRACE("err: initialize_audio_fifo failed\n");
			return fail_load();
		}
	}

	// init decoder
	frame = av_frame_alloc();
	filt_frame = av_frame_alloc();
	packet = av_packet_alloc();
	if (!frame || !filt_frame || !packet)
	{
		NATIVE_TRACE("err: allocate decode frame or packet failed\n");
		return fail_load();
	}

	reset_av_filter_equalizer();
	if (init_av_filter_equalizer() < 0)
	{
		uninitialize_audio_engine();
		release_audio_context();
		return -1;
	}

	init_decoder_thread();
	return 0;
}

void MusicPlayerLibrary::MusicPlayer::release_audio_context()
{
	if (avio_context)
	{
		avio_context_free(&avio_context);
		avio_context = nullptr;
		buffer = nullptr;
	}
	else if (buffer)
	{
		av_freep(&buffer);
		buffer = nullptr;
	}
	if (format_context)
	{
		// 释放文件解析上下文
		avformat_close_input(&format_context);
		format_context = nullptr;
	}

	if (codec_context)
	{
		// 释放解码器上下文
		avcodec_free_context(&codec_context);
		codec_context = nullptr;
	}
	uninitialize_audio_fifo();
	if (file_stream)
	{
		file_stream.reset();
	}
}

void MusicPlayerLibrary::MusicPlayer::reset_audio_context()
{
	// release_audio_context();
	file_stream_end = false;
	// 坑！坑！坑！equalizer线程的循环条件是user_request_stop == true，此处不重置会导致没人提交fifo数据！！！
	user_request_stop.store(false);
	if (is_audio_context_initialized()) {
		stop_audio_decode();
		bool snapshot_published;
		{
			std::lock_guard graph_lock(filter_graph_mutex);
			++equalizer_reset_generation;
			snapshot_published = publish_equalizer_snapshot_locked();
		}
		if (!snapshot_published)
		{
			NATIVE_TRACE("err: publish equalizer reset snapshot failed\n");
			playback_state.store(audio_playback_state_stopped);
			return;
		}
		av_seek_frame(format_context, audio_stream_index, 0, AVSEEK_FLAG_BACKWARD);
		avcodec_flush_buffers(codec_context);
		// 重置滤镜图
		NATIVE_TRACE("info: audio context reset, rebuilding filter graph\n");
		reset_av_filter_equalizer();
		if (init_av_filter_equalizer() < 0)
		{
			playback_state.store(audio_playback_state_stopped);
			return;
		}
	}
	playback_state.store(audio_playback_state_init);
	reset_audio_fifo();
	if (fft_executer)
		fft_executer->ResetBuffers();
	init_decoder_thread();
	// load_audio_context_from_file_stream();
}

bool MusicPlayerLibrary::MusicPlayer::is_audio_context_initialized()
{
	return avio_context
		&& format_context
		&& codec_context
		&& file_stream;
}

std::vector<std::uint8_t> MusicPlayerLibrary::MusicPlayer::get_id3_album_art_stream(const int stream_index)
{
	if (!format_context) return {};

	// stream_index = attached pic
	const AVPacket& pkt = format_context->streams[stream_index]->attached_pic;
	if (pkt.data == nullptr || pkt.size <= 0)
		return {};
	return {pkt.data, pkt.data + static_cast<std::size_t>(pkt.size)};
}

void MusicPlayerLibrary::MusicPlayer::require_download_ncm_album_art(const std::wstring& url)
{
	publish_message({PlayerMessageType::NcmAlbumArtDownloadRequired, url});
}

void MusicPlayerLibrary::MusicPlayer::publish_message(PlayerMessage message) const
{
	if (message.type == PlayerMessageType::TimeChanged && suppress_time_events.load())
		return;
	if (message_sink_)
		message_sink_->Publish(message);
}

void MusicPlayerLibrary::MusicPlayer::read_metadata()
{
	auto convert_utf8 = [](const char* utf_8_str) {
		return LocaleConverter::GetUtf16StringFromUtf8String(
			utf_8_str == nullptr ? std::string() : std::string(utf_8_str));
		};
	auto read_metadata_iter = [&](const AVDictionaryEntry* tag) {
		std::wstring key = convert_utf8(tag->key);
		std::wstring value = convert_utf8(tag->value);
		if (EqualsIgnoreCase(key, L"title") && song_title.empty()) {
			song_title = value;
			NATIVE_TRACE(L"info: song title: %s\n", song_title.c_str());
		}
		else if (EqualsIgnoreCase(key, L"artist") && song_artist.empty()) {
			song_artist = value;
			NATIVE_TRACE(L"info: song artist: %s\n", song_artist.c_str());
		}
		else
		{
			if (const auto lower = ToLower(key); lower.find(L"lyric") != std::wstring::npos)
			{
				this->id3_string_lyric = value;
				NATIVE_TRACE("info: song contains lyric in metadata\n");
			}
			else
			{
				NATIVE_TRACE(L"info: key %s = %s\n", key.c_str(), value.c_str());
			}
		}
	};

	const AVDictionaryEntry* tag = nullptr;
	while ((tag = av_dict_get(format_context->metadata, "", tag, AV_DICT_IGNORE_SUFFIX))) {
		read_metadata_iter(tag);
	}

	// decode album title & artist
	for (unsigned int i = 0; i < format_context->nb_streams; i++) {
		AVStream* stream = format_context->streams[i];
		tag = nullptr;
		while ((tag = av_dict_get(stream->metadata, "", tag, AV_DICT_IGNORE_SUFFIX))) {
			read_metadata_iter(tag);
		}
	}
}

// playback area
bool MusicPlayerLibrary::MusicPlayer::wait_frame_ready(std::chrono::milliseconds timeout)
{
	std::unique_lock lock(frame_event_mutex);
	if (!frame_ready_cv.wait_for(lock, timeout, [this]
		{
			return frame_ready_requested || playback_state.load() == audio_playback_state_stopped;
		}))
	{
		return false;
	}
	frame_ready_requested = false;
	return true;
}

bool MusicPlayerLibrary::MusicPlayer::wait_frame_underrun(std::chrono::milliseconds timeout)
{
	std::unique_lock lock(frame_event_mutex);
	if (!frame_underrun_cv.wait_for(lock, timeout, [this]
		{
			return frame_underrun_requested || playback_state.load() == audio_playback_state_stopped;
		}))
	{
		return false;
	}
	frame_underrun_requested = false;
	return true;
}

void MusicPlayerLibrary::MusicPlayer::notify_frame_ready()
{
	{
		std::lock_guard lock(frame_event_mutex);
		frame_ready_requested = true;
	}
	frame_ready_cv.notify_one();
}

void MusicPlayerLibrary::MusicPlayer::notify_frame_underrun()
{
	{
		std::lock_guard lock(frame_event_mutex);
		frame_underrun_requested = true;
	}
	frame_underrun_cv.notify_one();
}

void MusicPlayerLibrary::MusicPlayer::reset_frame_notifications()
{
	std::lock_guard lock(frame_event_mutex);
	frame_ready_requested = false;
	frame_underrun_requested = false;
}

void MusicPlayerLibrary::MusicPlayer::notify_all_frame_notifications()
{
	{
		std::lock_guard lock(frame_event_mutex);
		frame_ready_requested = true;
		frame_underrun_requested = true;
	}
	frame_ready_cv.notify_all();
	frame_underrun_cv.notify_all();
	decoded_frame_queue_cv.notify_all();
}

int MusicPlayerLibrary::MusicPlayer::get_audio_fifo_low_watermark()
{
	return static_cast<int>(sample_rate / 48000.00 * faudio_play_frame_size);
}

int MusicPlayerLibrary::MusicPlayer::get_audio_fifo_high_watermark()
{
	return (std::max)(faudio_play_frame_size * 4, get_audio_fifo_low_watermark() * 2);
}

int MusicPlayerLibrary::MusicPlayer::get_decoded_frame_queue_high_watermark()
{
	const int queue_sample_rate = sample_rate > 0 ? sample_rate : 48000;
	return (std::max)(queue_sample_rate, faudio_play_frame_size * 64);
}

bool MusicPlayerLibrary::MusicPlayer::is_audio_pipeline_running()
{
	return decoder_is_running.load() || equalizer_is_running.load();
}

bool MusicPlayerLibrary::MusicPlayer::queue_decoded_frame(AVFrame* decoded_frame)
{
	if (!decoded_frame)
		return false;

	UniqueAVFrame queued_frame(av_frame_alloc());
	if (!queued_frame)
	{
		FFMPEG_CRITICAL_ERROR(AVERROR(ENOMEM));
		return false;
	}
	av_frame_move_ref(queued_frame.get(), decoded_frame);
	const int frame_samples = queued_frame->nb_samples;

	std::unique_lock lock(decoded_frame_queue_mutex);
	while (!decoded_frame_queue_abort
		&& playback_state.load() != audio_playback_state_stopped
		&& decoded_frame_queue_samples >= get_decoded_frame_queue_high_watermark())
	{
		decoded_frame_queue_cv.wait_for(lock, std::chrono::milliseconds(2));
	}

	if (decoded_frame_queue_abort || playback_state.load() == audio_playback_state_stopped)
	{
		return false;
	}

	decoded_frame_queue.push_back(queued_frame.get());
	queued_frame.release();
	decoded_frame_queue_samples += frame_samples;
	lock.unlock();
	decoded_frame_queue_cv.notify_all();
	return true;
}

AVFrame* MusicPlayerLibrary::MusicPlayer::pop_decoded_frame()
{
	std::unique_lock lock(decoded_frame_queue_mutex);
	decoded_frame_queue_cv.wait(lock, [this]
		{
			return decoded_frame_queue_abort
				|| playback_state.load() == audio_playback_state_stopped
				|| !decoded_frame_queue.empty()
				|| decoded_frame_queue_eof;
		});

	if (decoded_frame_queue_abort || playback_state.load() == audio_playback_state_stopped)
		return nullptr;
	if (decoded_frame_queue.empty())
		return nullptr;

	AVFrame* frame = decoded_frame_queue.front();
	decoded_frame_queue.pop_front();
	decoded_frame_queue_samples -= frame ? frame->nb_samples : 0;
	if (decoded_frame_queue_samples < 0)
		decoded_frame_queue_samples = 0;
	lock.unlock();
	decoded_frame_queue_cv.notify_all();
	return frame;
}

void MusicPlayerLibrary::MusicPlayer::signal_decoded_frame_queue_eof()
{
	{
		std::lock_guard lock(decoded_frame_queue_mutex);
		decoded_frame_queue_eof = true;
	}
	decoded_frame_queue_cv.notify_all();
}

void MusicPlayerLibrary::MusicPlayer::reset_decoded_frame_queue(bool abort_waiters)
{
	std::lock_guard lock(decoded_frame_queue_mutex);
	for (AVFrame*& queued_frame : decoded_frame_queue)
	{
		av_frame_free(&queued_frame);
	}
	decoded_frame_queue.clear();
	decoded_frame_queue_samples = 0;
	decoded_frame_queue_eof = false;
	decoded_frame_queue_abort = abort_waiters;
	decoded_frame_queue_cv.notify_all();
}

inline int MusicPlayerLibrary::MusicPlayer::initialize_audio_engine()
{
	if (!codec_context)
		return -1;

	// COM-based WIC calls replaced by libSkiaSharp in C# layer.
	// C++ layer now only encapsulates raw picture bytes and submit payload to C# layer.

	// create com obj
	if (FAudioCallFailed(FAudioCreate(&faudio, 0, FAUDIO_DEFAULT_PROCESSOR)))
	{
		NATIVE_TRACE("err: create faudio object failed\n");
		uninitialize_audio_engine();
		return -1;
	}

	// create mastering voice
	if (FAudioCallFailed(FAudio_CreateMasteringVoice(faudio, &mastering_voice,
		2,
		sample_rate,
		0, 0, nullptr))) {
		NATIVE_TRACE("err: creating mastering voice failed\n");
		uninitialize_audio_engine();
		return -1;
	}


	// 创建source voice
	wfx.wFormatTag = FAUDIO_FORMAT_PCM;                     // pcm格式
	wfx.nChannels = 2;                                    // 音频通道数
	wfx.nSamplesPerSec = sample_rate;                           // 采样率
	wfx.wBitsPerSample = 16;  // xaudio2支持16-bit pcm，如果不符合格式的音频，使用swscale进行转码
	wfx.nBlockAlign = (wfx.wBitsPerSample / 8) * wfx.nChannels; // 样本大小：样本大小(16-bit)*通道数
	wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign; // 每秒钟解码多少字节，样本大小*采样率
	wfx.cbSize = sizeof(wfx);
	AudioDsp::EqualizerDspSnapshot initial_equalizer_snapshot;
	{
		std::lock_guard graph_lock(filter_graph_mutex);
		initial_equalizer_snapshot = build_equalizer_snapshot_locked();
	}

	FAudioSourceVoice* local_source_voice = nullptr;
	if (FAudioCallFailed(FAudio_CreateSourceVoice(faudio,
		&local_source_voice,
		&wfx, FAUDIO_VOICE_NOPITCH,
		2.0, nullptr, nullptr, nullptr)))
	{
		NATIVE_TRACE("err: create source voice failed\n");
		uninitialize_audio_engine();
		return -1;
	}
	auto fail_local_source_voice = [this, &local_source_voice]() {
		if (local_source_voice)
		{
			FAudioVoice_DestroyVoice(local_source_voice);
			local_source_voice = nullptr;
		}
		uninitialize_audio_engine();
		return -1;
		};

	FAPO* equalizer_fapo = nullptr;
	const AudioDsp::LimiterConfig limiter{
		.enabled = true,
		.ceiling = 0.70f,
		.lookahead_ms = 5.0f,
		.release_ms = 50.0f
	};
	const std::uint32_t create_fapo_result = AudioDsp::CreateEqualizerFapo(
		initial_equalizer_snapshot, limiter, &equalizer_fapo);
	if (create_fapo_result != FAUDIO_OK || !equalizer_fapo)
	{
		NATIVE_TRACE("err: create equalizer FAPO failed, reason=0x%x\n", create_fapo_result);
		if (equalizer_fapo)
			equalizer_fapo->Release(equalizer_fapo);
		return fail_local_source_voice();
	}

	FAudioEffectDescriptor equalizer_effect{
		.pEffect = equalizer_fapo,
		.InitialState = 1,
		.OutputChannels = wfx.nChannels
	};
	FAudioEffectChain effect_chain{
		.EffectCount = 1,
		.pEffectDescriptors = &equalizer_effect
	};
	const std::uint32_t set_chain_result = FAudioVoice_SetEffectChain(
		local_source_voice, &effect_chain);
	if (set_chain_result != FAUDIO_OK)
	{
		equalizer_fapo->Release(equalizer_fapo);
		equalizer_fapo = nullptr;
		NATIVE_TRACE("err: attach equalizer FAPO failed, reason=0x%x\n", set_chain_result);
		return fail_local_source_voice();
	}
	equalizer_fapo->Release(equalizer_fapo);
	equalizer_fapo = nullptr;

	std::uint32_t prewarm_result;
	{
		std::lock_guard graph_lock(filter_graph_mutex);
		const auto latest_snapshot = build_equalizer_snapshot_locked();
		prewarm_result = FAudioVoice_SetEffectParameters(
			local_source_voice, 0, &latest_snapshot, sizeof(latest_snapshot), FAUDIO_COMMIT_NOW);
		if (prewarm_result == FAUDIO_OK)
		{
			source_voice = local_source_voice;
			local_source_voice = nullptr;
		}
	}
	if (prewarm_result != FAUDIO_OK)
	{
		NATIVE_TRACE("err: prewarm equalizer FAPO parameters failed, reason=0x%x\n", prewarm_result);
		return fail_local_source_voice();
	}

	last_frametime = 0.0;
	standard_frametime = faudio_play_frame_size * 1.0 / wfx.nSamplesPerSec * 1000; // in ms
	playback_state.store(audio_playback_state_init);
	// init FFTExecuter
	try
	{
		fft_executer = new FFTExecuter(wfx.nSamplesPerSec);
	}
	catch (const std::exception& e)
	{
		NATIVE_TRACE("err: create fft executer failed, reason=%s\n", e.what());
		uninitialize_audio_engine();
		return -1;
	}
	
	return 0;
}

inline void MusicPlayerLibrary::MusicPlayer::uninitialize_audio_engine()
{
	// 等待xaudio线程执行完成
	if (audio_player_worker_thread.joinable())
	{
		playback_state.store(audio_playback_state_stopped);
		audio_player_worker_thread.request_stop();
		notify_all_frame_notifications();
		audio_player_worker_thread.join();
	}
	stop_audio_decode();
	reset_decoded_frame_queue(true);
	FAudioSourceVoice* local_source_voice = nullptr;
	{
		std::lock_guard graph_lock(filter_graph_mutex);
		local_source_voice = source_voice;
		source_voice = nullptr;
	}
	if (local_source_voice) {
		(void)FAudioSourceVoice_Stop(local_source_voice, 0, FAUDIO_COMMIT_NOW);
		(void)FAudioSourceVoice_FlushSourceBuffers(local_source_voice);
		FAudioVoice_DestroyVoice(local_source_voice);
	}
	if (mastering_voice) {
		FAudioVoice_DestroyVoice(mastering_voice);
		mastering_voice = nullptr;
	}
	if (faudio) {
		FAudio_Release(faudio);
		faudio = nullptr;
	}
	if (frame) {
		av_frame_free(&frame);
		frame = nullptr;
	}
	if (filt_frame)
	{
		av_frame_free(&filt_frame);
		filt_frame = nullptr;
	}
	if (packet) {
		av_packet_free(&packet);
		packet = nullptr;
	}
	if (fft_executer)
	{
		delete fft_executer;
		fft_executer = nullptr;
	}
	// release xaudio2 buffer
	faudio_free_buffer();
	faudio_destroy_buffer();
}

void MusicPlayerLibrary::MusicPlayer::audio_playback_worker_thread()
{
	uint32_t hr;
	FAudioVoiceState state;

	while (true) {
		if (!wait_frame_ready(std::chrono::milliseconds(1))) {
			// check flag
			int cached_sample_size = get_audio_fifo_cached_samples_size();
			if (playback_state.load() == audio_playback_state_stopped) {
				break;
			}
			if (playback_state.load() == audio_playback_state_init ||
				playback_state.load() == audio_playback_state_decoder_exit_pre_stop ||
				cached_sample_size > get_audio_fifo_low_watermark()) {
				// pass
				if (cached_sample_size < get_audio_fifo_low_watermark()) {
					notify_frame_underrun();
				}
			}
			else if (file_stream_end) {
				NATIVE_TRACE("info: decode stopped, fetch from fifo\n");
				notify_frame_ready(); // avoid deadlock
			}
			else {
				notify_frame_underrun();
				continue;
			}
		}
		// clock_t decode_begin_time = clock();

		std::lock_guard playback_lock(audio_playback_mutex);

		int fifo_size = get_audio_fifo_cached_samples_size();
		if (fifo_size < 0 && is_audio_pipeline_running()) {
			// LeaveCriticalSection(audio_playback_section);
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			continue;
		}
		if (playback_state.load() == audio_playback_state_decoder_exit_pre_stop) {
			// bypass
		}
		else if (!is_audio_pipeline_running() && fifo_size == 0) {
			NATIVE_TRACE("info: audio pipeline stopped and fifo empty, ending playback thread\n");
			playback_state.store(audio_playback_state_decoder_exit_pre_stop);
			continue;
		}
		// if (fifo_size < xaudio2_play_frame_size) {
		// 	SetEvent(frame_underrun_event);
		// 	LeaveCriticalSection(audio_playback_section);
		//	Sleep(1);
		//	continue;
		// }
//			playback_state->store(audio_playback_state_stopped);


		FAudioSourceVoice_GetState(source_voice, &state, 0);
		if (user_request_stop.load()) {
			// immediate return
			NATIVE_TRACE("info: user request stop, do cleaning\n");

			base_offset = state.SamplesPlayed;
			break;
		}
		if (playback_state.load() ==
			audio_playback_state_decoder_exit_pre_stop)
		{

			if (fifo_size == 0 && state.BuffersQueued > 0)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
				FAudioSourceVoice_GetState(source_voice, &state, 0);
				elapsed_time = static_cast<float>(state.SamplesPlayed - base_offset) * 1.0f / static_cast<float>(wfx.nSamplesPerSec) + static_cast<float>(pts_seconds);

				publish_message({PlayerMessageType::TimeChanged, elapsed_time.load()});
				// AfxGetMainWnd()->PostMessage(WM_PLAYER_TIME_CHANGE, raw);
				continue;
			}
			
			NATIVE_TRACE("info: playback finished, destroying thread\n");
			publish_message({PlayerMessageType::Stopped});
			// AfxGetMainWnd()->PostMessage(WM_PLAYER_STOP);
			base_offset = state.SamplesPlayed;
			faudio_played_samples = 0;
			faudio_played_buffers = 0;
			// fix pts_seconds not clear up -> ui thread time error & resume failed
			pts_seconds = 0.0;
			playback_state.store(audio_playback_state_stopped);
			// elapsed_time = 0.0;
			// AfxGetMainWnd()->PostMessage(WM_PLAYER_TIME_CHANGE, raw);
			// EnterCriticalSection(audio_playback_section);
			// bool need_clean = !user_request_stop;
			// LeaveCriticalSection(audio_playback_section);
			// if (need_clean)
			// 	reset_audio_context();
			break; // 读取结束
		}

		// Read XAudio2-ready PCM directly from the FIFO. The filter graph already
		// converts to stereo/s16/sample_rate, so sample counts are in output frames.
		AVSamplesBuffer fifo_buf;
		int read_samples;
		{
			std::lock_guard fifo_lock(audio_fifo_mutex);
			fifo_size = get_audio_fifo_cached_samples_size();
			if (fifo_size <= 0)
			{
				notify_frame_underrun();
				continue;
			}
			if (is_audio_pipeline_running() && !file_stream_end && fifo_size < faudio_play_frame_size)
			{
				notify_frame_underrun();
				continue;
			}
			const int fifo_read_size = (std::min)(faudio_play_frame_size, fifo_size);
			if (int alloc_ret = fifo_buf.allocate(fifo_audio_channels, fifo_read_size, fifo_audio_sample_fmt);
				alloc_ret < 0)
			{
				playback_state.store(audio_playback_state_stopped);
				FFMPEG_CRITICAL_ERROR(alloc_ret);
				break;
			}
			read_samples = read_samples_from_fifo(fifo_buf.get(), fifo_read_size);
			if (read_samples < 0) {
				NATIVE_TRACE("err: read samples from fifo failed, code=%d\n", read_samples);
				NATIVE_TRACE("err: fifo size=%d", get_audio_fifo_cached_samples_size());
				if (user_request_stop.load())
				{
					NATIVE_TRACE("info: user request stop and fifo cleared up, exiting\n");
					break;
				}
				fifo_buf.reset();
				playback_state.store(audio_playback_state_stopped);
				FFMPEG_CRITICAL_ERROR(read_samples);
				// LeaveCriticalSection(audio_fifo_section);
				// LeaveCriticalSection(audio_playback_section);
				break;
			}
		}

		if (read_samples == 0)
		{
			NATIVE_TRACE("info: no samples read, spin wait instead\n");
			std::this_thread::sleep_for(std::chrono::milliseconds(5)); // wait for producing buffer
			continue;
		}
		const uint32_t audio_bytes = read_samples * wfx.nBlockAlign;

		while (state.BuffersQueued >= 32)
		{
			if (user_request_stop.load())
				break;
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			FAudioSourceVoice_GetState(source_voice, &state, 0);
		}
		if (user_request_stop.load())
		{
			break;
		}

		// 将滤镜图输出的PCM数据提交到XAudio2
		FAudioBuffer* buffer_pcm = faudio_get_available_buffer(audio_bytes);
		buffer_pcm->AudioBytes = audio_bytes; // 16-bit stereo, bytes per sample-frame = wfx.nBlockAlign
		memcpy(const_cast<std::uint8_t*>(buffer_pcm->pAudioData), fifo_buf.first_plane(), buffer_pcm->AudioBytes);
		fifo_buf.reset();

		hr = FAudioSourceVoice_SubmitSourceBuffer(source_voice, buffer_pcm, nullptr);
		if (FAudioCallFailed(hr)) {
			NATIVE_TRACE("err: submit source buffer failed, reason=0x%x\n", hr);
			playback_state.store(audio_playback_state_stopped);
			break;
		}

		// Visualization is best-effort and observes the pre-FAPO PCM after it is queued.
		if (fft_executer)
		{
			fft_executer->AddSamplesToRingBuffer(
				buffer_pcm->pAudioData,
				static_cast<int>(audio_bytes));
		}

		if (playback_state.load() == audio_playback_state_init)
		{
			// if (state.BuffersQueued == 32)
			// {
			playback_state.store(audio_playback_state_playing);
			FAudioSourceVoice_Start(source_voice, 0, FAUDIO_COMMIT_NOW);
			publish_message({PlayerMessageType::Started});
			// AfxGetMainWnd()->PostMessage(WM_PLAYER_START);
			std::this_thread::sleep_for(std::chrono::milliseconds(5)); // wait for consuming buffer
			// }
		}

		FAudioSourceVoice_GetState(source_voice, &state, 0);
		// std::printf("info: submitted source buffer, buffers queued=%d\n", state.BuffersQueued);

		// 播放音频
		// source_voice->GetState(&state);
		// if (*playback_state == audio_playback_state_init)
		// {
			// if (state.BuffersQueued == 32)
			// {
			//	InterlockedExchange(playback_state, audio_playback_state_playing);
			//	source_voice->Start();
			// 	AfxGetMainWnd()->PostMessage(WM_PLAYER_START);
			// }
		// }
		// else
		// {
			// fix: avoid crash
		const size_t queued_buffers = state.BuffersQueued;
		if (faudio_playing_buffers.size() > queued_buffers)
		{
			const size_t played_count = faudio_playing_buffers.size() - queued_buffers;
			auto played_end = faudio_playing_buffers.begin();
			for (size_t i = 0; i < played_count; ++i)
			{
				faudio_played_samples += (*played_end)->AudioBytes / wfx.nBlockAlign;
				faudio_played_buffers++;
				++played_end;
			}
			faudio_free_buffers.insert(faudio_free_buffers.end(),
				faudio_playing_buffers.begin(), played_end);
			faudio_playing_buffers.erase(faudio_playing_buffers.begin(), played_end);
		}

		if (fft_executer)
		{
			int latency_bytes = std::transform_reduce(faudio_playing_buffers.begin(), faudio_playing_buffers.end(), 
				0, 
				std::plus{},
				[](auto buf) {
					return buf->AudioBytes;
				}
			);
			// Samples = Bytes / nBlockAlign
			int latency_samples = latency_bytes / wfx.nBlockAlign;
			// Time = Samples / SamplesPerSec
			int latency_ms = static_cast<int>(
				static_cast<double>(latency_samples) * 1000.0 / wfx.nSamplesPerSec
			);
			// Compensate XAudio2 output latency by timestamp instead of frame count.
			fft_executer->SetOutputDelayMilliseconds(latency_ms);
		}
		double decode_time_ms = static_cast<double>(faudio_played_samples - prev_decode_cycle_faudio_played_samples) *
			1000.0 / wfx.nSamplesPerSec;
		prev_decode_cycle_faudio_played_samples = faudio_played_samples;
		elapsed_time = static_cast<double>(faudio_played_samples) * 1.0 / wfx.nSamplesPerSec + pts_seconds.load();

		// clock_t decode_end_time = clock();
		// double decode_time_ms = (decode_end_time - decode_begin_time) * 1000.0 / CLOCKS_PER_SEC;
		// remove duplicate log
		// ATLTRACE("info: xaudio2 cpu time %lf ms , frame time %lf ms!\n",
		//	 decode_time_ms, standard_frametime);
		// limit msg freq to 60mps, avoid ui stuck
		if (message_interval_timer > message_interval
			|| message_interval_timer < 0.0f)
		{
			message_interval_timer = 0.0f;
			publish_message({PlayerMessageType::TimeChanged, elapsed_time.load()});
		}
		else { message_interval_timer += static_cast<float>(decode_time_ms); }
		// else
		// {
			// std::printf("info: buffer played=%zd\n", xaudio2_played_buffers);
		// }
		//  (wfx.wBitsPerSample / 8) * wfx.nChannels
	// }

	// LeaveCriticalSection(audio_playback_section);
	// EnterCriticalSection(audio_fifo_section);
		std::lock_guard fifo_event_lock(audio_fifo_mutex);
		if (get_audio_fifo_cached_samples_size() < get_audio_fifo_low_watermark()) {
			// need more data
			NATIVE_TRACE("info: audio fifo cached samples size=%d, frame underrun!\n", get_audio_fifo_cached_samples_size());
			notify_frame_underrun();
		}
		else if (state.BuffersQueued < 16) {
			// enough data buffered
			notify_frame_ready();
		}
		// LeaveCriticalSection(audio_fifo_section);
	}
}

void MusicPlayerLibrary::MusicPlayer::audio_decode_worker_thread()
{
	bool is_eof = false;
	bool decoder_flushed = false;
	while (true) {
		if (playback_state.load() == audio_playback_state_stopped) {
			NATIVE_TRACE("info: playback stopped, decoder thread exiting\n");
			break;
		}

		{
			std::unique_lock queue_lock(decoded_frame_queue_mutex);
			if (decoded_frame_queue_samples >= get_decoded_frame_queue_high_watermark())
			{
				decoded_frame_queue_cv.wait_for(queue_lock, std::chrono::milliseconds(2), [this]
					{
						return decoded_frame_queue_abort
							|| playback_state.load() == audio_playback_state_stopped
							|| decoded_frame_queue_samples < get_decoded_frame_queue_high_watermark();
					});
				if (decoded_frame_queue_abort || playback_state.load() == audio_playback_state_stopped)
					break;
				if (decoded_frame_queue_samples >= get_decoded_frame_queue_high_watermark())
					continue;
			}
		}

		if (playback_state.load() == audio_playback_state_init
			&& is_pause) {
			NATIVE_TRACE("info: resume from pause, pts_seconds=%lf\n", pts_seconds.load());
			if (av_seek_frame(format_context, -1, static_cast<int64_t>(pts_seconds * AV_TIME_BASE), AVSEEK_FLAG_ANY) < 0) {
				NATIVE_TRACE("err: resume failed\n");
				playback_state.store(audio_playback_state_stopped);
			}
			avcodec_flush_buffers(codec_context);
			is_pause = false;
			is_eof = false;
			decoder_flushed = false;
		}

		// 从输入文件中读取数据并解码
		if (!is_eof) {
			if (av_read_frame(format_context, packet) < 0) {
				NATIVE_TRACE("info: av_read_frame reached eof, entering flush mode\n");
				// 文件流结束，进入flush模式
				is_eof = true;
			}
			else if (packet->stream_index != audio_stream_index) {
				notify_frame_underrun();
				av_packet_unref(packet);
				continue; // 跳过非音频流包
			}
		}
		// is_eof后，不单独检查packet->stream_index。此时此值无意义。
		
		if (is_eof && !decoder_flushed) {
			// 发送空包以排空解码器缓存
			if (int ret = avcodec_send_packet(codec_context, nullptr); ret < 0 && ret != AVERROR_EOF) {
				NATIVE_TRACE("warn: flush decoder failed, code=%d\n", ret);
			}
			decoder_flushed = true;
		}
		else if (!is_eof) {
			// 正常送入数据包
			if (int ret = avcodec_send_packet(codec_context, packet); ret < 0) {
				if (ret == AVERROR_INVALIDDATA) {
					// 忽略坏块
					av_packet_unref(packet);
					continue;
				}
				av_packet_unref(packet);
				playback_state.store(audio_playback_state_stopped);
				FFMPEG_CRITICAL_ERROR(ret);
				break;
			}
		}
		while (true)
		{
			if (int receive_res = avcodec_receive_frame(codec_context, frame); receive_res == AVERROR(EAGAIN)) {
				break; // 没有更多帧
			}
			else
			{
				if (receive_res == AVERROR_EOF) {
					NATIVE_TRACE("info: decoder flushed, signaling equalizer eof\n");
					signal_decoded_frame_queue_eof();
					av_frame_unref(frame);
					if (!is_eof) {
						av_packet_unref(packet);
					}
					return;
				}
				if (receive_res < 0) {
					av_frame_unref(frame);
					playback_state.store(audio_playback_state_stopped);
					FFMPEG_CRITICAL_ERROR(receive_res);
					break;
				}
			}
			if (!queue_decoded_frame(frame))
			{
				if (playback_state.load() != audio_playback_state_stopped)
				{
					NATIVE_TRACE("err: queue decoded frame failed\n");
					playback_state.store(audio_playback_state_stopped);
				}
				break;
			}
		}

		av_frame_unref(frame); // eof, err process -> proper unref
		if (!is_eof) {
			// EOF模式时，发送的包是空包
			av_packet_unref(packet);
		}
	}

	if (playback_state.load() != audio_playback_state_stopped)
		signal_decoded_frame_queue_eof();
}

void MusicPlayerLibrary::MusicPlayer::audio_equalize_worker_thread()
{
	auto wait_for_fifo_room = [this]
		{
			while (playback_state.load() != audio_playback_state_stopped && !user_request_stop.load())
			{
				int fifo_size;
				{
					std::lock_guard fifo_lock(audio_fifo_mutex);
					fifo_size = get_audio_fifo_cached_samples_size();
				}
				if (fifo_size < get_audio_fifo_high_watermark())
					return true;

				wait_frame_underrun(std::chrono::milliseconds(2));
			}
			return false;
		};

	auto drain_filter_to_fifo = [this]()
		{
			while (true)
			{
				const int res = av_buffersink_get_frame(filter_context_sink, filt_frame);
				if (res == AVERROR(EAGAIN))
					// 此时滤镜图已被排空，返回true以从decoded_frame_queue中取出下一帧
					return true;
				if (res == AVERROR_EOF)
				{
					NATIVE_TRACE("info: filter flushed, all samples processed\n");
					file_stream_end = true;
					return false;
				}
				if (res < 0)
				{
					FFMPEG_CRITICAL_ERROR(res);
					playback_state.store(audio_playback_state_stopped);
					return false;
				}

				{
					std::lock_guard fifo_lock(audio_fifo_mutex);
					if (int ret_code = add_samples_to_fifo(filt_frame->extended_data, filt_frame->nb_samples);
						ret_code < 0)
					{
						av_frame_unref(filt_frame);
						playback_state.store(audio_playback_state_stopped);
						FFMPEG_CRITICAL_ERROR(ret_code);
						return false;
					}
				}
				av_frame_unref(filt_frame);
				notify_frame_ready();
			}
		};

	bool filter_flush_sent = false;
	while (playback_state.load() != audio_playback_state_stopped && !user_request_stop.load())
	{
		if (!wait_for_fifo_room())
			break;

		UniqueAVFrame decoded_frame(pop_decoded_frame());
		if (!decoded_frame)
		{
			bool reached_eof;
			{
				std::lock_guard queue_lock(decoded_frame_queue_mutex);
				reached_eof = decoded_frame_queue_eof
					&& decoded_frame_queue.empty()
					&& !decoded_frame_queue_abort;
			}

			if (!reached_eof)
				break;

			std::lock_guard graph_lock(filter_graph_mutex);
			if (!filter_flush_sent)
			{
				NATIVE_TRACE("info: decoded queue eof, flushing audio filter graph\n");
				if (int ret = av_buffersrc_add_frame(filter_context_src, nullptr); ret < 0 && ret != AVERROR_EOF)
				{
					FFMPEG_CRITICAL_ERROR(ret);
					playback_state.store(audio_playback_state_stopped);
					break;
				}
				filter_flush_sent = true;
			}
			if (!drain_filter_to_fifo())
				break;
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			continue;
		}

		{
			std::lock_guard graph_lock(filter_graph_mutex);
			if (int add_frame_ret = av_buffersrc_add_frame(filter_context_src, decoded_frame.get()); add_frame_ret < 0)
			{
				decoded_frame.reset();
				playback_state.store(audio_playback_state_stopped);
				if (add_frame_ret != AVERROR_EOF) {
					FFMPEG_CRITICAL_ERROR(add_frame_ret);
				}
				else {
					NATIVE_TRACE("info: filter shutdown, exiting\n");
				}
				break;
			}
			decoded_frame.reset();
			if (!drain_filter_to_fifo())
				break;
		}


		bool decoded_queue_has_room;
		{
			std::lock_guard queue_lock(decoded_frame_queue_mutex);
			decoded_queue_has_room = !decoded_frame_queue_abort
				&& decoded_frame_queue_samples < get_decoded_frame_queue_high_watermark();
		}
		if (decoded_queue_has_room)
			decoded_frame_queue_cv.notify_all();

		int player_buffers_queued = is_faudio_initialized()
			                             ? decoder_query_faudio_buffer_size()
			                             : 0;
		if (player_buffers_queued < 4 && playback_state.load() == audio_playback_state_playing) {
			NATIVE_TRACE("info: xaudio2 buffers queued=%d, notify player thread to submit data\n", player_buffers_queued);
			notify_frame_ready();
		}
	}
	notify_frame_ready();
	notify_frame_underrun();
}

void MusicPlayerLibrary::MusicPlayer::handle_worker_exception(const std::string& message, const char* worker_name)
{
	NATIVE_TRACE("err: audio %s worker failed\n", worker_name);
	playback_state.store(audio_playback_state_stopped);
	user_request_stop.store(true);
	decoder_is_running = false;
	equalizer_is_running = false;
	{
		std::lock_guard queue_lock(decoded_frame_queue_mutex);
		decoded_frame_queue_abort = true;
	}
	decoded_frame_queue_cv.notify_all();
	notify_all_frame_notifications();

	publish_message({PlayerMessageType::Error, message});
}



void MusicPlayerLibrary::MusicPlayer::init_decoder_thread() {
	if (audio_decoder_worker_thread.joinable() || audio_equalizer_worker_thread.joinable())
	{
		stop_audio_decode();
	}
	file_stream_end = false;
	reset_decoded_frame_queue(false);
	init_equalizer_thread();
	decoder_is_running = true;
	audio_decoder_worker_thread = std::jthread(
		[this] {
			std::unique_ptr<IAudioThreadScheduleHelper> mmcss = 
				GetDefaultAudioThreadSchedulerFactory()->CreateAudioThreadScheduleHelper(
					L"Audio", MPL_AUDIO_PRIORITY::MPL_AUDIO_PRIORITY_HIGH, "decoder"
					);
			try
			{
				audio_decode_worker_thread();
			}
			catch (const std::exception& exception)
			{
				std::string message = std::format("Unhandled native exception in decoder worker: {}", exception.what());
				handle_worker_exception(message, "decoder");
				return;
			}
			catch (...)
			{
				handle_worker_exception("Unhandled unknown exception in decoder worker.", "decoder");
				return;
			}
			decoder_is_running = false;
		});
	NATIVE_TRACE("info: decoder thread created\n");
	notify_frame_underrun();
}

void MusicPlayerLibrary::MusicPlayer::init_equalizer_thread()
{
	if (audio_equalizer_worker_thread.joinable())
	{
		stop_audio_equalizer();
	}
	equalizer_is_running = true;
	audio_equalizer_worker_thread = std::jthread(
		[this] {
			std::unique_ptr<IAudioThreadScheduleHelper> mmcss = 
				GetDefaultAudioThreadSchedulerFactory()->CreateAudioThreadScheduleHelper(
					L"Audio", MPL_AUDIO_PRIORITY::MPL_AUDIO_PRIORITY_HIGH, "equalizer"
			);
			try
			{
				audio_equalize_worker_thread();
			}
			catch (const std::exception& exception)
			{
				std::string message = std::format("Unhandled native exception in equalizer worker: {}", exception.what());
				handle_worker_exception(message, "equalizer");
				return;
			}
			catch (...)
			{
				handle_worker_exception("Unhandled unknown exception in equalizer worker.", "equalizer");
				return;
			}
			equalizer_is_running = false;
		});
	NATIVE_TRACE("info: equalizer thread created\n");
}

inline void MusicPlayerLibrary::MusicPlayer::start_audio_playback()
{
	if (playback_state.load() == audio_playback_state_stopped) {
		reset_audio_context();
		if (playback_state.load() == audio_playback_state_stopped)
		{
			NATIVE_TRACE("err: audio context reset failed, playback will not start\n");
			return;
		}
	}
	if (source_voice) {
		FAudioVoiceState state;
		FAudioSourceVoice_GetState(source_voice, &state, 0);
		base_offset = state.SamplesPlayed;
	}
	playback_state.store(audio_playback_state_init);
	message_interval_timer = -1.0f;
	if (audio_player_worker_thread.joinable())
	{
		stop_audio_playback(0);
		if (playback_state.load() == audio_playback_state_stopped)
		{
			NATIVE_TRACE("err: audio context reset failed, playback will not restart\n");
			return;
		}
	}
	if (fft_executer)
		fft_executer->ResetBuffers();
	user_request_stop.store(false);
	audio_player_worker_thread = std::jthread(
		[this] {
			std::unique_ptr<IAudioThreadScheduleHelper> mmcss = 
				GetDefaultAudioThreadSchedulerFactory()->CreateAudioThreadScheduleHelper(
					L"Pro Audio", MPL_AUDIO_PRIORITY::MPL_AUDIO_PRIORITY_CRITICAL, "playback"
			);
			try
			{
				audio_playback_worker_thread();
			}
			catch (const std::exception& exception)
			{
				std::string message = std::format("Unhandled native exception in playback worker: {}", exception.what());
				handle_worker_exception(message, "playback");
			}
			catch (...)
			{
				handle_worker_exception("Unhandled unknown exception in playback worker.", "playback");
			}
		});
	NATIVE_TRACE("info: player thread created\n");
	// notify decoder to start decoding
}

void MusicPlayerLibrary::MusicPlayer::stop_audio_decode(int mode)
{
	if (audio_decoder_worker_thread.joinable() || audio_equalizer_worker_thread.joinable())
	{
		playback_state.store(audio_playback_state_stopped);
		{
			std::lock_guard queue_lock(decoded_frame_queue_mutex);
			decoded_frame_queue_abort = true;
		}
		decoded_frame_queue_cv.notify_all();
		notify_all_frame_notifications();
		if (audio_decoder_worker_thread.joinable())
		{
			audio_decoder_worker_thread.request_stop();
			audio_decoder_worker_thread.join();
		}
		stop_audio_equalizer();
	}
	decoder_is_running = false;
	equalizer_is_running = false;
}

void MusicPlayerLibrary::MusicPlayer::stop_audio_equalizer()
{
	if (audio_equalizer_worker_thread.joinable())
	{
		{
			std::lock_guard queue_lock(decoded_frame_queue_mutex);
			decoded_frame_queue_abort = true;
		}
		decoded_frame_queue_cv.notify_all();
		notify_all_frame_notifications();
		audio_equalizer_worker_thread.request_stop();
		audio_equalizer_worker_thread.join();
	}
	equalizer_is_running = false;
	reset_decoded_frame_queue(true);
}

void MusicPlayerLibrary::MusicPlayer::stop_audio_playback(int mode)
{
	if (audio_player_worker_thread.joinable())
	{
		// 播放线程在排空fifo时可能长期持有锁，此处持有锁会导致暂停时ui冻结。
		// 使用std::atomic_bool存储标志位，并在播放线程的几个关键节点手动检测。
		// *重构的时候才想起TryEnterCriticalSection的好。
		user_request_stop.store(true);
		playback_state.store(audio_playback_state_stopped);
		notify_all_frame_notifications();
		audio_player_worker_thread.request_stop();
		audio_player_worker_thread.join();

		if (source_voice)
		{
			(void)FAudioSourceVoice_Stop(source_voice, 0, FAUDIO_COMMIT_NOW);
			(void)FAudioSourceVoice_FlushSourceBuffers(source_voice);
			
			FAudioVoiceState state;
			FAudioSourceVoice_GetState(source_voice, &state, 0);
			
			while (state.BuffersQueued > 0)
			{
				FAudioSourceVoice_GetState(source_voice, &state, 0);
			}
		}
	}
	// Stop decoder after playback thread exits; pause/reset can then rebuild FIFO
	// and filters without racing the playback worker.
	stop_audio_decode(is_pause ? 1 : 0);
	// terminated faudio and ffmpeg, do cleanup
	faudio_free_buffer();
	faudio_destroy_buffer();
	faudio_played_samples = faudio_played_buffers = faudio_played_samples = faudio_played_buffers = 0;
	double pts_time_d;
	if (is_pause)
	{
		pts_time_d = pts_seconds.load();
	}
	else {
		elapsed_time = pts_time_d = 0.0;
	}
	suppress_time_events = false;
	publish_message({PlayerMessageType::TimeChanged, pts_time_d});
	// AfxGetMainWnd()->PostMessage(WM_PLAYER_TIME_CHANGE, raw);
	reset_frame_notifications();
	if (mode == 0)
		reset_audio_context();
	else if (mode == -1)
		release_audio_context();
}

int MusicPlayerLibrary::MusicPlayer::initialize_audio_fifo(AVSampleFormat sample_fmt, int channels, int nb_samples)
{
	audio_fifo = av_audio_fifo_alloc(sample_fmt, channels, nb_samples);
	if (!audio_fifo)
	{
		// AfxMessageBox(_T("err: could not allocate audio fifo!"), MB_ICONERROR);
		FFMPEG_CRITICAL_ERROR(-1);
		return -1;
	}
	return 0;
}

int MusicPlayerLibrary::MusicPlayer::resize_audio_fifo(int nb_samples)
{
	if (!audio_fifo)
		return -1;
	if (int ret_value; (ret_value = av_audio_fifo_realloc(audio_fifo, nb_samples)) < 0) {
		FFMPEG_CRITICAL_ERROR(ret_value);
		return ret_value;
	}
	return 0;
}

int MusicPlayerLibrary::MusicPlayer::add_samples_to_fifo(uint8_t** decoded_data, int nb_samples)
{
	if (!audio_fifo)
		return -1;
	if (int res = av_audio_fifo_write(audio_fifo, reinterpret_cast<void**>(decoded_data), nb_samples); res < 0) {
		// audio fifo will resize automatically
		FFMPEG_CRITICAL_ERROR(res);
		return res;
	}
	// 	ATLTRACE("info: added %d samples to audio fifo\n", res);
	return 0;
}

int MusicPlayerLibrary::MusicPlayer::read_samples_from_fifo(uint8_t** output_buffer, int nb_samples)
{
	int ret;
	if (!audio_fifo)
		return -1;
	if ((ret = av_audio_fifo_read(audio_fifo, reinterpret_cast<void**>(output_buffer), nb_samples)) < 0) {
		FFMPEG_CRITICAL_ERROR(ret);
		return -1;
	}
	return ret;
}

void MusicPlayerLibrary::MusicPlayer::drain_audio_fifo(int nb_samples)
{
	if (!audio_fifo)
		return;
	if (int ret; (ret = av_audio_fifo_drain(audio_fifo, nb_samples)) < 0) {
		FFMPEG_CRITICAL_ERROR(ret);
	}
}

void MusicPlayerLibrary::MusicPlayer::reset_audio_fifo()
{
	if (!audio_fifo)
		return;
	av_audio_fifo_reset(audio_fifo);
}

int MusicPlayerLibrary::MusicPlayer::get_audio_fifo_cached_samples_size()
{
	if (!audio_fifo)
		return -1;
	return av_audio_fifo_size(audio_fifo);
}

void MusicPlayerLibrary::MusicPlayer::uninitialize_audio_fifo()
{
	if (audio_fifo)
	{
		av_audio_fifo_free(audio_fifo);
		audio_fifo = nullptr;
	}
}

inline const char* MusicPlayerLibrary::MusicPlayer::get_backend_implement_version() // NOLINT(*-convert-member-functions-to-static)
{
	static char faudio_implement_version[] = "XAudio 2.8 Compatible";
	return faudio_implement_version;
}

void MusicPlayerLibrary::MusicPlayer::faudio_init_buffer(FAudioBuffer* dest_buffer, int size) // NOLINT(*-convert-member-functions-to-static)
{
	if (size < 8192) size = 8192;
	if (int& buffer_size = *reinterpret_cast<int*>(dest_buffer->pContext); size > buffer_size)
	{
		NATIVE_TRACE("info: faudio reallocate_buffer, reallocate_size=%d, original_size=%d\n", size, buffer_size);
		std::uint8_t* new_audio_data = DBG_NEW std::uint8_t[size];
		delete[] dest_buffer->pAudioData;
		dest_buffer->pAudioData = new_audio_data;
		buffer_size = size;
	}
	memset(const_cast<std::uint8_t*>(dest_buffer->pAudioData), 0, size);
}

FAudioBuffer* MusicPlayerLibrary::MusicPlayer::faudio_allocate_buffer(int size)
{
	if (size < 8192) size = 8192;
	// ATLTRACE("info: xaudio2_allocate_buffer, allocate_size=%d\n", size);
	UniqueFAudioBuffer dest_buffer(DBG_NEW FAudioBuffer{}); // NOLINT(*-use-auto)
	std::unique_ptr<std::uint8_t[]> audio_data(DBG_NEW std::uint8_t[size]);
	std::unique_ptr<int> context(DBG_NEW int(size));
	dest_buffer->pAudioData = audio_data.release();
	dest_buffer->pContext = context.release();
	faudio_init_buffer(dest_buffer.get());
	return dest_buffer.release();
}

FAudioBuffer* MusicPlayerLibrary::MusicPlayer::faudio_get_available_buffer(int size)
{
	// std::printf("info: DBG_NEW xaudio2_buffer request, allocated=%lld, played=%lld\n", xaudio2_allocated_buffers, xaudio2_played_buffers);
	if (!faudio_free_buffers.empty())
	{
		// std::printf("info: free buffer recycled\n");
		auto dest_buffer = faudio_free_buffers.front();
		faudio_init_buffer(dest_buffer, size);
		faudio_playing_buffers.push_back(dest_buffer);
		faudio_free_buffers.pop_front();
		return dest_buffer;
	}
	// Allocate a new XAudio2 buffer.
	UniqueFAudioBuffer new_buffer(faudio_allocate_buffer(size));
	faudio_playing_buffers.push_back(new_buffer.get());
	faudio_allocated_buffers++;
	// std::printf("info: new xaudio2 buffer allocated, current allocate: %lld\n", xaudio2_allocated_buffers);
	return new_buffer.release();
}

void MusicPlayerLibrary::MusicPlayer::faudio_free_buffer()
{
	for (auto& i : faudio_playing_buffers)
	{
		assert(i);
		DeleteFAudioBuffer(i);
		i = nullptr;
	}
	faudio_allocated_buffers = 0; faudio_played_buffers = 0;
	faudio_playing_buffers.clear();
}

void MusicPlayerLibrary::MusicPlayer::faudio_destroy_buffer()
{
	for (auto& i : faudio_free_buffers)
	{
		assert(i);
		DeleteFAudioBuffer(i);
		i = nullptr;
	}
	faudio_free_buffers.clear();
}

int MusicPlayerLibrary::MusicPlayer::decoder_query_faudio_buffer_size()
{
	std::lock_guard lock(audio_playback_mutex);
	FAudioVoiceState state;
	FAudioSourceVoice_GetState(source_voice, &state, 0);
	int buffer_size = static_cast<int>(state.BuffersQueued);
	return buffer_size;
}

bool MusicPlayerLibrary::MusicPlayer::is_faudio_initialized()
{
	return wfx.nSamplesPerSec > 0 && wfx.nBlockAlign > 0
		&& source_voice && mastering_voice && faudio;
}

size_t MusicPlayerLibrary::MusicPlayer::get_samples_played_per_session()
{
	FAudioVoiceState state;
	FAudioSourceVoice_GetState(source_voice, &state, 0);
	return state.SamplesPlayed - base_offset;
}

void MusicPlayerLibrary::MusicPlayer::dialog_ffmpeg_critical_error(int err_code, const char* file, int line) // NOLINT(*-convert-member-functions-to-static)
{
	char buf[1024] = { 0 };
	av_strerror(err_code, buf, 1024);
	std::string detail = std::format("FFmpeg critical error: {} (file: {}, line: {})\n", buf, file, line);
	throw std::runtime_error(detail);
}

MusicPlayerLibrary::MusicPlayer::MusicPlayer(IMusicPlayerMessageSink* message_sink) :
	playback_state(audio_playback_state_init),
	message_sink_(message_sink)
{
	NATIVE_TRACE("info: decode frontend: avformat version %d, avcodec version %d, avutil version %d, swresample version %d\n",
		avformat_version(),
		avcodec_version(),
		avutil_version(),
		swresample_version());
	NATIVE_TRACE("info: audio api backend: FAudio, version %s\n", get_backend_implement_version());
}

/**
 * @brief Initializes the audio filter graph using libavfilter
 *
 * @note The graph is owned by the equalizer worker. The playback FIFO buffers
 * normalized pre-FAPO PCM for a short window.
 */
int MusicPlayerLibrary::MusicPlayer::init_av_filter_equalizer()
{
	std::lock_guard graph_lock(filter_graph_mutex);
	filter_graph = avfilter_graph_alloc();
	if (!filter_graph)
	{
		NATIVE_TRACE("err: avfilter_graph_alloc failed\n");
		return -1;
	}
	auto release_filter_graph = [this]() noexcept {
		if (filter_graph)
			avfilter_graph_free(&filter_graph);
		filter_graph = nullptr;
		filter_context_src = filter_context_sink = nullptr;
		resample_ctx = nullptr;
		volume_ctx = format_normalize_ctx = nullptr;
		};
	auto fail_filter = [&release_filter_graph](int code) {
		release_filter_graph();
		return code;
		};
	auto fail_filter_with_ffmpeg = [&release_filter_graph, this](int code) {
		release_filter_graph();
		FFMPEG_CRITICAL_ERROR(code);
		return code;
		};

	if (sample_rate <= 0)
		sample_rate = 48000;

	std::string layout_str(256, '\0');
	av_channel_layout_describe(&codec_context->ch_layout, layout_str.data(), layout_str.size());
	layout_str.resize(strlen(layout_str.c_str()));
	const char* sample_fmt_name = av_get_sample_fmt_name(codec_context->sample_fmt);
	std::string args = std::format("sample_rate={}:sample_fmt={}:channel_layout={}",
		codec_context->sample_rate,
		sample_fmt_name == nullptr ? "" : sample_fmt_name,
		layout_str);
	NATIVE_TRACE("info: init_av_filter_equalizer, filter args: %s\n", args.c_str());
	int ret = avfilter_graph_create_filter(&filter_context_src, avfilter_get_by_name("abuffer"),
		"src", args.c_str(), nullptr, filter_graph);
	if (ret < 0)
	{
		return fail_filter_with_ffmpeg(ret);
	}

	std::string resample_args = std::format("sample_rate={}:out_chlayout=stereo:out_sample_fmt=s16", sample_rate);
	ret = avfilter_graph_create_filter(&resample_ctx, avfilter_get_by_name("aresample"),
		"resample", resample_args.c_str(), nullptr, filter_graph);
	if (ret < 0)
	{
		return fail_filter_with_ffmpeg(ret);
	}
	NATIVE_TRACE("info: resample filter created, param = %s\n", resample_args.c_str());

	ret = avfilter_graph_create_filter(&filter_context_sink, avfilter_get_by_name("abuffersink"),
		"sink", nullptr, nullptr, filter_graph);
	if (ret < 0)
	{
		return fail_filter_with_ffmpeg(ret);
	}
	ret = avfilter_graph_create_filter(&volume_ctx, avfilter_get_by_name("volume"),
		"pregain", "volume=0.7", nullptr, filter_graph);
	if (ret < 0)
	{
		return fail_filter_with_ffmpeg(ret);
	}
	std::string fmt_args = std::format("sample_fmts=s16:sample_rates={}:channel_layouts=stereo", sample_rate);
	ret = avfilter_graph_create_filter(&format_normalize_ctx,
		avfilter_get_by_name("aformat"),
		"aformat", fmt_args.c_str(), nullptr, filter_graph);
	if (ret < 0)
	{
		return fail_filter_with_ffmpeg(ret);
	}
	NATIVE_TRACE("info: format filter created, param = %s\n", fmt_args.c_str());
	if ((ret = avfilter_link(filter_context_src, 0, resample_ctx, 0)) < 0 ||
		(ret = avfilter_link(resample_ctx, 0, volume_ctx, 0)) < 0 ||
		(ret = avfilter_link(volume_ctx, 0, format_normalize_ctx, 0)) < 0 ||
		(ret = avfilter_link(format_normalize_ctx, 0, filter_context_sink, 0)) < 0)
	{
		return fail_filter_with_ffmpeg(ret);
	}

	ret = avfilter_graph_config(filter_graph, nullptr);
	if (ret < 0)
	{
		return fail_filter_with_ffmpeg(ret);
	}

	const AVFilterLink* sink_link = filter_context_sink->inputs[0];
	const int sink_channels = sink_link->ch_layout.nb_channels;
	NATIVE_TRACE("info: filter output format=%s, sample_rate=%d, channels=%d\n",
		av_get_sample_fmt_name(static_cast<AVSampleFormat>(sink_link->format)),
		sink_link->sample_rate,
		sink_channels);
	if (sink_link->sample_rate != sample_rate ||
		sink_channels != fifo_audio_channels ||
		sink_link->format != fifo_audio_sample_fmt)
	{
		NATIVE_TRACE("err: filter output format mismatch, expected format=s16, sample_rate=%d, channels=%d\n",
			sample_rate, fifo_audio_channels);
		return fail_filter(-1);
	}
	return 0;
}

bool MusicPlayerLibrary::MusicPlayer::is_av_filter_equalizer_initialized()
{
	return filter_context_src && filter_context_sink;
}

void MusicPlayerLibrary::MusicPlayer::reset_av_filter_equalizer()
{
	std::lock_guard graph_lock(filter_graph_mutex);
	if (filter_graph)
		avfilter_graph_free(&filter_graph);
	filter_graph = nullptr;
	filter_context_src = filter_context_sink = nullptr;
	resample_ctx = nullptr;
	volume_ctx = format_normalize_ctx = nullptr;
}

bool MusicPlayerLibrary::MusicPlayer::IsInitialized()
{
	return is_audio_context_initialized() && is_faudio_initialized();
}

bool MusicPlayerLibrary::MusicPlayer::IsPlaying()
{
	return IsInitialized() &&
		playback_state.load() != audio_playback_state_init && playback_state.load() != audio_playback_state_stopped;
}

void MusicPlayerLibrary::MusicPlayer::OpenFile(const std::wstring& fileName, const std::wstring& file_extension_in, bool skip_album_art_loading)
{
	auto cleanup_failed_open = [this]() {
		uninitialize_audio_engine();
		reset_av_filter_equalizer();
		release_audio_context();
		};
	try
	{
		if (load_audio_context(fileName, file_extension_in, skip_album_art_loading)) {
			// AfxMessageBox(_T("err: load file failed, please check trace message!"), MB_ICONERROR);
			throw std::runtime_error("Load file failed, please re-run in terminal and check trace message!");
		}
		if (initialize_audio_engine()) {
			// AfxMessageBox(_T("err: audio engine initialize failed!"), MB_ICONERROR);
			throw std::runtime_error("Audio engine initialize failed!");
		};
		publish_message({PlayerMessageType::FileInitialized});
		publish_message({PlayerMessageType::TimeChanged, 0.0});
		// AfxGetMainWnd()->PostMessage(WM_PLAYER_FILE_INIT);
	}
	catch (...)
	{
		cleanup_failed_open();
		throw;
	}
}

double MusicPlayerLibrary::MusicPlayer::GetMusicTimeLength()
{
	if (IsInitialized()) {
		if (fabs(length.load() - 0.0) < 0.0001) {
			AVStream* audio_stream = format_context->streams[audio_stream_index];
			int64_t duration = audio_stream->duration;
			AVRational time_base = audio_stream->time_base;
			length = static_cast<double>(duration) * av_q2d(time_base);
		}
		return length;
	}
	return 0.0f;
}

double MusicPlayerLibrary::MusicPlayer::GetCurrentMusicPosition()
{
	if (IsInitialized())
	{
		return elapsed_time.load();
	}
	return 0.0;
}

std::wstring MusicPlayerLibrary::MusicPlayer::GetSongTitle()
{
	if (IsInitialized()) {
		return song_title;
	}
	return {};
}

std::wstring MusicPlayerLibrary::MusicPlayer::GetSongArtist()
{
	if (IsInitialized()) {
		return song_artist;
	}
	return {};
}

void MusicPlayerLibrary::MusicPlayer::Start()
{
	if (IsInitialized() && !IsPlaying()) {
		start_audio_playback();
	}
}

void MusicPlayerLibrary::MusicPlayer::Stop()
{
	if (IsInitialized() && IsPlaying()) {
		pts_seconds = 0;
		stop_audio_playback(0);
		publish_message({PlayerMessageType::Stopped});
	}
}

void MusicPlayerLibrary::MusicPlayer::SetMasterVolume(float volume)
{
	if (IsInitialized()) {
		if (volume < 0.0f) volume = 0.0f;
		if (volume > 1.0f) volume = 1.0f;
		(void)FAudioVoice_SetVolume(mastering_voice, volume, FAUDIO_COMMIT_NOW);
	}
}

void MusicPlayerLibrary::MusicPlayer::SeekToPosition(double time, bool need_stop)
{
	if (IsInitialized()) {
		is_pause = true;
		pts_seconds = time;
		if (IsInitialized())
		{
			if (need_stop && (IsPlaying() || audio_player_worker_thread.joinable()))
			{
				suppress_time_events = true;
				user_request_stop.store(true);
				stop_audio_playback(0);
			}
			else if (!IsPlaying()) {
				if (decoder_is_running) {
					stop_audio_decode(1);
					playback_state.store(audio_playback_state_init);
					reset_audio_context();
					publish_message({PlayerMessageType::TimeChanged, time});
				}
			}
		}
	}
}

// 根据奈奎斯特采样定律，建议选择44.1kHz以上的采样率获得完整听觉体验
void MusicPlayerLibrary::MusicPlayer::SetSampleRate(int sample_rate)
{
	if (IsInitialized()) {
		// Set sample rate after init is not supported.
		throw std::logic_error("SetSampleRate is not supported after initialization!");
	}
	this->sample_rate = sample_rate;
}

int MusicPlayerLibrary::MusicPlayer::GetNBlockAlign()
{
	return wfx.nBlockAlign;
}

std::wstring MusicPlayerLibrary::MusicPlayer::GetID3Lyric()
{
	return id3_string_lyric;
}

MusicPlayerLibrary::AudioDsp::EqualizerDspSnapshot
MusicPlayerLibrary::MusicPlayer::build_equalizer_snapshot_locked() const noexcept
{
	return AudioDsp::CompileEqualizerSnapshot(
		equalizer_config, sample_rate, equalizer_reset_generation);
}

bool MusicPlayerLibrary::MusicPlayer::publish_equalizer_snapshot_locked() noexcept
{
	if (!source_voice)
		return true;
	const auto snapshot = build_equalizer_snapshot_locked();
	return FAudioVoice_SetEffectParameters(
		source_voice, 0, &snapshot, sizeof(snapshot), FAUDIO_COMMIT_NOW) == 0;
}

int MusicPlayerLibrary::MusicPlayer::GetEqualizerBand(int index)
{
	if (index < 0 || static_cast<std::size_t>(index) >= AudioDsp::EqualizerBandCount)
		return 0;
	std::lock_guard graph_lock(filter_graph_mutex);
	return static_cast<int>(equalizer_config.bands[index].gain_db);
}

void MusicPlayerLibrary::MusicPlayer::SetEqualizerBand(int index, int value)
{
	if (index < 0 || static_cast<std::size_t>(index) >= AudioDsp::EqualizerBandCount)
		return;
	value = std::clamp(value, -24, 24);
	bool snapshot_published;
	{
		std::lock_guard graph_lock(filter_graph_mutex);
		equalizer_config.bands[index].gain_db = static_cast<float>(value);
		snapshot_published = publish_equalizer_snapshot_locked();
	}
	if (!snapshot_published)
		NATIVE_TRACE("err: publish equalizer snapshot failed for band %d\n", index);
}

void MusicPlayerLibrary::MusicPlayer::Pause()
{
	if (IsInitialized() && IsPlaying()) {
		is_pause = true;
		pts_seconds = elapsed_time.load();
		stop_audio_playback(0);
		publish_message({PlayerMessageType::Paused});
	}
}

MusicPlayerLibrary::MusicPlayer::~MusicPlayer()
{
	if (playback_state.load() == audio_playback_state_playing) {
		user_request_stop.store(true);
		stop_audio_playback(-1);
	}
	stop_audio_decode();
	uninitialize_audio_engine();

	if (audio_fifo) 				uninitialize_audio_fifo();
	reset_av_filter_equalizer();
	release_audio_context();

	if (file_stream)
	{
		file_stream->Close();
		file_stream.reset();
	}
}
