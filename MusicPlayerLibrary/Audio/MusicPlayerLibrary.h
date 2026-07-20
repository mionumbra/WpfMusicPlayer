// SPDX-License-Identifier: MIT

#pragma once

#include <list>
#include <deque>
#include <optional>

#if defined(__cplusplus)
extern "C" {
#endif
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/imgutils.h>
#include <libavutil/frame.h>
#include <libavfilter/avfilter.h>

#if defined(__cplusplus)
}
#endif

#include "Core/NativeTraceRedirect.h"
#include <atomic>
#include <FAudio.h>
#include "Audio/AudioOutputFormat.h"
#include "Audio/DSP/EqualizerDsp.h"
#include "Audio/FFT/FFTExecuter.h"
#include "Audio/FFT/AudioPipelinePerformanceHelper.h"
#include "Audio/MusicPlayerMessage.h"
#include "Core/FileAbstractionLayer.h"

namespace MusicPlayerLibrary {
	

	enum audio_playback_state : int
	{
		audio_playback_state_init,
		audio_playback_state_playing,
		audio_playback_state_paused,
		audio_playback_state_decoder_exit_pre_stop,
		audio_playback_state_stopped
	};

	class MusicPlayer
	{
		// 流文件解析上下文
		AVFormatContext* format_context = nullptr;
		// 针对该文件，找到的解码器类型
		AVCodec* codec = nullptr;
		// 使用的解码器实例
		AVCodecContext* codec_context = nullptr;
		// 解码前的数据（流中的一个packet）
		AVPacket* packet = nullptr;
		// 解码后的数据（一帧数据）
		AVFrame* frame = nullptr;
		AVFrame* normalized_frame = nullptr;
		// 音频流编号
		// fix: audio_stream_index can < 0;
		// using unsigned cause it to overflow, as a very huge number
		// crashing the audio engine.
		int audio_stream_index = -1;
		AVIOContext* avio_context = nullptr;
		unsigned char* buffer = nullptr;

		std::wstring file_extension;
		bool skip_album_art_loading = false;
		std::unique_ptr<IFile> file_stream;
		std::atomic_bool file_stream_end = false;
		std::atomic_bool user_request_stop = false;
		std::atomic<double> pts_seconds = 0.0;
		std::atomic<double> elapsed_time = 0.0;
		std::atomic<double> length = 0.0;
		std::atomic_bool is_pause = false;
		std::atomic_bool decoder_is_running = false;
		std::atomic_bool normalizer_is_running = false;
		int fifo_audio_channels = 0;
		AVSampleFormat fifo_audio_sample_fmt = AV_SAMPLE_FMT_NONE;
		int fifo_sample_rate = 0;
		AudioFormatInfo audio_source_format_info{};
		AudioBitrateTracker audio_source_bitrate_tracker;
		std::atomic<double> average_audio_bitrate_bits_per_second{ 0.0 };
		std::atomic_bool is_loseless_audio{ false };
		std::atomic_bool is_hi_res_audio{ false };
		std::wstring song_title = {};
		std::wstring song_artist = {};

		std::mutex decoded_frame_queue_mutex;
		std::condition_variable decoded_frame_queue_cv;
		std::deque<AVFrame*> decoded_frame_queue;
		int decoded_frame_queue_samples = 0;
		bool decoded_frame_queue_eof = false;
		bool decoded_frame_queue_abort = false;
		std::atomic_int decoded_frame_queue_high_watermark_samples{ 1'536 };

		std::mutex audio_fifo_mutex;
		std::mutex audio_pipeline_buffering_mutex;
		std::mutex audio_playback_mutex;
		std::mutex filter_graph_mutex;
		std::mutex frame_event_mutex;
		std::condition_variable frame_ready_cv;
		std::condition_variable frame_underrun_cv;
		bool frame_ready_requested = false;
		bool frame_underrun_requested = false;

		FAudio* faudio = nullptr;
		FAudioMasteringVoice* mastering_voice = nullptr;
		FAudioSourceVoice* source_voice = nullptr;
		AudioOutputFormat audio_output_format{};

		std::atomic_int playback_state;
		std::jthread audio_player_worker_thread;
		std::jthread audio_decoder_worker_thread;
		std::jthread audio_normalizer_worker_thread;

		std::list<FAudioBuffer*> faudio_playing_buffers = {};
		std::list<FAudioBuffer*> faudio_free_buffers = {};
		size_t faudio_played_buffers = 0, faudio_allocated_buffers = 0, faudio_played_samples = 0;
		size_t base_offset = 0;

		// use avaudiofifo to avoid lag on low-cpu performance system, like jasper lake/alder lake-n
		AVAudioFifo* audio_fifo = nullptr;
		int faudio_play_frame_size = 256;
		AudioPipelineBufferingProfile initial_buffering_profile{};
		std::atomic_int audio_fifo_high_watermark_samples{ 6'720 };
		std::atomic_uint decoder_slow_operation_count{ 0 };
		std::atomic_uint normalizer_slow_operation_count{ 0 };
		std::atomic<std::int64_t> last_pipeline_buffer_growth_ns{ 0 };
		double decoder_pending_work_milliseconds = 0.0;
		int decoder_last_call_result = 0;
		double standard_frametime = 0.0, last_frametime = 0.0;
		float message_interval = 16.67f, message_interval_timer = 0.0f;
		size_t prev_decode_cycle_faudio_played_samples = 0;
		std::wstring id3_string_lyric;
		IMusicPlayerMessageSink* message_sink_ = nullptr;

		// file I/O Area
		int read_func(uint8_t* buf, int buf_size);
		static int read_func_wrapper(void* opaque, uint8_t* buf, int buf_size);
		int64_t seek_func(int64_t offset, int whence);
		static int64_t seek_func_wrapper(void* opaque, int64_t offset, int whence);
		int load_audio_context(const std::wstring& audio_filename, const std::wstring& file_extension_in = {}, bool skip_album_art_loading = false);
		int load_audio_context_from_file_stream();
		void release_audio_context();
		void reset_audio_context();
		int get_average_audio_bitrate() const;
		void reset_audio_quality_flags() noexcept;
		void update_audio_quality_flags(
			std::uint64_t stream_length_bytes,
			int source_sample_rate) noexcept;
		bool is_audio_context_initialized();
		std::vector<std::uint8_t> get_id3_album_art_stream(int stream_index);
		void require_download_ncm_album_art(const std::wstring& url);
		void publish_message(PlayerMessage message) const;
		void read_metadata();

		// playback area
		int initialize_audio_engine();
		void init_decoder_thread();
		void uninitialize_audio_engine();
		bool wait_frame_ready(std::chrono::milliseconds timeout);
		bool wait_frame_underrun(std::chrono::milliseconds timeout);
		void notify_frame_ready();
		void notify_frame_underrun();
		void reset_frame_notifications();
		void notify_all_frame_notifications();
		void audio_playback_worker_thread();
		int timed_read_packet();
		int timed_send_decoder_packet(const AVPacket* input_packet);
		int timed_receive_decoded_frame();
		void reset_audio_source_bitrate() noexcept;
		void observe_audio_source_packet(const AVPacket& source_packet) noexcept;
		void observe_audio_source_frame(const AVFrame& decoded_frame) noexcept;
		void audio_decode_worker_thread();
		void audio_normalize_worker_thread();
		void handle_worker_exception(const std::string& message, const char* worker_name);
		void start_audio_playback();
		void init_normalizer_thread();
		void stop_audio_decode(int mode = 0);
		void stop_audio_normalizer();
		void stop_audio_playback(int mode);
		bool is_audio_pipeline_running();
		enum class audio_pipeline_stage
		{
			decoder,
			normalizer
		};
		void reset_audio_pipeline_buffering();
		void observe_audio_pipeline_stage_duration(
			audio_pipeline_stage stage,
			double elapsed_milliseconds,
			int processed_samples,
			int processed_sample_rate);
		void grow_audio_pipeline_buffers(
			const char* slow_stage,
			double elapsed_milliseconds,
			double slow_threshold_milliseconds);

		int initialize_audio_fifo(AVSampleFormat sample_fmt, int channels, int nb_samples);
		int resize_audio_fifo(int nb_samples);
		int add_samples_to_fifo(uint8_t** decoded_data, int nb_samples);
		int read_samples_from_fifo(uint8_t** output_buffer, int nb_samples);
		void drain_audio_fifo(int nb_samples);
		void reset_audio_fifo();
		int get_audio_fifo_cached_samples_size();
		void uninitialize_audio_fifo();
		int get_audio_fifo_low_watermark();
		int get_audio_fifo_high_watermark();
		int get_decoded_frame_queue_high_watermark();
		bool queue_decoded_frame(AVFrame* decoded_frame);
		AVFrame* pop_decoded_frame();
		void signal_decoded_frame_queue_eof();
		void reset_decoded_frame_queue(bool abort_waiters = false);

		// FAudio helper function
		const char* get_backend_implement_version();
		void faudio_init_buffer(FAudioBuffer* dest_buffer, int size = 8192);
		FAudioBuffer* faudio_allocate_buffer(int size = 8192);
		FAudioBuffer* faudio_get_available_buffer(int size = 8192);
		void faudio_free_buffer();
		void faudio_destroy_buffer();
		int decoder_query_faudio_buffer_size();
		bool is_faudio_initialized();
		size_t get_samples_played_per_session();
	public:
		// using WriteRawPCMBytesCallback = std::function<void(const uint8_t* buffer_out, int buffer_size)>;
		FFTExecuter* fft_executer = nullptr;
	protected:

		// debug function
		void dialog_ffmpeg_critical_error(int err_code, const char* file, int line);

		// equalizer settings
		AudioDsp::EqualizerConfig equalizer_config =
			AudioDsp::MakeDefaultTenBandConfig();
		std::uint64_t equalizer_reset_generation = 0;
		AVFilterGraph* normalization_filter_graph = nullptr;
		AVFilterContext* normalization_source_context = nullptr;
		AVFilterContext* normalization_sink_context = nullptr;
		AVFilterContext* resample_context = nullptr;
		AVFilterContext* pregain_context = nullptr;
		AVFilterContext* format_normalization_context = nullptr;

		AudioDsp::EqualizerDspSnapshot
			build_equalizer_snapshot_locked() const noexcept;
		bool publish_equalizer_snapshot_locked() noexcept;

		int init_audio_normalization_filter();
		bool is_audio_normalization_filter_initialized();
		void reset_audio_normalization_filter();
		std::atomic_bool suppress_time_events = false;
	public:

		// constructor
		explicit MusicPlayer(
			IMusicPlayerMessageSink* message_sink = nullptr,
			std::optional<AudioOutputFormat> output_format = std::nullopt);
		// no copy & move
		MusicPlayer(const MusicPlayer&) = delete;
		MusicPlayer& operator=(const MusicPlayer&) = delete;
		MusicPlayer(MusicPlayer&&) = delete;
		MusicPlayer& operator=(MusicPlayer&&) = delete;

		// Interfaces
		bool IsInitialized();
		bool IsPlaying();
		void OpenFile(const std::wstring& fileName, const std::wstring& file_extension_in = {}, bool skip_album_art_loading = false);
		double GetMusicTimeLength();
		double GetCurrentMusicPosition();
		std::wstring GetSongTitle();
		std::wstring GetSongArtist();
		// Returns raw decoded-source metadata. For lossless/PCM sources the
		// stream's effective bit depth is preferred; formats without a declared
		// bit depth fall back to the decoder's sample representation.
		[[nodiscard]] AudioFormatInfo GetAudioSourceFormatInfo() const noexcept;
		// Returns raw metadata for the resolved format submitted to the device.
		[[nodiscard]] AudioFormatInfo GetDeviceOutputFormatInfo() const noexcept;
		// Returns the cumulative elementary-stream payload byte rate observed in
		// the current continuous decode epoch. Container overhead is excluded.
		// The unit is decimal KByte/s (1 KByte = 1000 bytes).
		[[nodiscard]] double GetAudioSourceBitrate() const noexcept;
		// Uses only the backing stream's average bitrate. The deliberately named
		// heuristic is true only above 800 kbit/s.
		[[nodiscard]] bool IsLoselessAudio() const noexcept;
		// True only when the decoded source sample rate is above 48 kHz.
		[[nodiscard]] bool IsHiResAudio() const noexcept;
		[[nodiscard]] int GetAverageAudioBitrate() const noexcept;
		void Start();
		void Pause();
		void Stop();
		void SetMasterVolume(float volume);
		void SeekToPosition(double time, bool need_stop);
		void SetSampleRate(int sample_rate);
		// int GetRawPCMBytes(uint8_t* buffer_out, int buffer_size) const;

		int GetNBlockAlign();
		std::wstring GetID3Lyric();

		// Equalizer interfaces
		int GetEqualizerBand(int index);
		void SetEqualizerBand(int index, int value);

		// destructor
		~MusicPlayer();
	};

}
