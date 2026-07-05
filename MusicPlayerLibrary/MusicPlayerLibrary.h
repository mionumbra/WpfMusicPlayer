// SPDX-License-Identifier: MIT

#pragma once

#include "NativeTraceRedirect.h"
#include <atomic>
#include <vcclr.h>
#include "FFTExecuter.h"
#include "FileAbstractionLayer.h"
using namespace System;

namespace MusicPlayerLibrary {

	public ref class NativeTraceRedirectManager {
		static NativeTraceRedirect* m_pRedirector;
		public:
			static void Init(System::Object^ logger);

	};

	public delegate void WriteRawPCMBytesCallback(array<uint8_t>^ buffer_out, int buffer_size);

	public enum audio_playback_state : int
	{
		audio_playback_state_init,
		audio_playback_state_playing,
		audio_playback_state_paused,
		audio_playback_state_decoder_exit_pre_stop,
		audio_playback_state_stopped
	};

	public enum MessageType : uint32_t {
		MPL_PLAYER_FILE_INIT = 100,
		MPL_PLAYER_TIME_CHANGE = 101,
		MPL_PLAYER_START = 102,
		MPL_PLAYER_PAUSE = 103,
		MPL_PLAYER_STOP = 104,
		MPL_PLAYER_ALBUM_ART_INIT = 105,
		MPL_PLAYER_NCM_REQUIRE_ALBUM_ART_DOWNLOAD = 106,
		MPL_PLAYER_DESTROY = 107,
		MPL_PLAYER_ERROR = 108
	};

	ref class MusicPlayer;
	public class MusicPlayerNative
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
		AVFrame* filt_frame = nullptr;
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
		std::atomic_bool equalizer_is_running = false;
		int fifo_audio_channels = 0;
		AVSampleFormat fifo_audio_sample_fmt = AV_SAMPLE_FMT_NONE;
		int fifo_sample_rate = 0;
		std::wstring song_title = {};
		std::wstring song_artist = {};

		std::mutex decoded_frame_queue_mutex;
		std::condition_variable decoded_frame_queue_cv;
		std::deque<AVFrame*> decoded_frame_queue;
		int decoded_frame_queue_samples = 0;
		bool decoded_frame_queue_eof = false;
		bool decoded_frame_queue_abort = false;

		std::mutex audio_fifo_mutex;
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
		FAudioWaveFormatEx wfx = {};

		std::atomic_int playback_state;
		std::jthread audio_player_worker_thread;
		std::jthread audio_decoder_worker_thread;
		std::jthread audio_equalizer_worker_thread;

		std::list<FAudioBuffer*> faudio_playing_buffers = {};
		std::list<FAudioBuffer*> faudio_free_buffers = {};
		size_t faudio_played_buffers = 0, faudio_allocated_buffers = 0, faudio_played_samples = 0;
		size_t base_offset = 0;

		// use avaudiofifo to avoid lag on low-cpu performance system, like jasper lake/alder lake-n
		AVAudioFifo* audio_fifo = nullptr;
		int faudio_play_frame_size = 256;
		double standard_frametime = 0.0, last_frametime = 0.0;
		float message_interval = 16.67f, message_interval_timer = 0.0f;
		size_t prev_decode_cycle_faudio_played_samples = 0;
		std::wstring id3_string_lyric;
		int sample_rate = 48000;

		// managed variables
		gcroot<MusicPlayer^> managed_music_player;

		// file I/O Area
		int read_func(uint8_t* buf, int buf_size);
		static int read_func_wrapper(void* opaque, uint8_t* buf, int buf_size);
		int64_t seek_func(int64_t offset, int whence);
		static int64_t seek_func_wrapper(void* opaque, int64_t offset, int whence);
		int load_audio_context(const std::wstring& audio_filename, const std::wstring& file_extension_in = {}, bool skip_album_art_loading = false);
		int load_audio_context_from_file_stream();
		void release_audio_context();
		void reset_audio_context();
		bool is_audio_context_initialized();
		array<System::Byte>^ get_id3_album_art_stream(int stream_index);
		void require_download_ncm_album_art(const std::wstring& url);
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
		void audio_decode_worker_thread();
		void audio_equalize_worker_thread();
		void handle_worker_exception(System::Exception^ exception, const char* worker_name);
		void start_audio_playback();
		void init_equalizer_thread();
		void stop_audio_decode(int mode = 0);
		void stop_audio_equalizer();
		void stop_audio_playback(int mode);
		bool is_audio_pipeline_running();

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
		std::vector<int> eq_bands;
		AVFilterGraph* filter_graph = nullptr;
		struct av_filter_eq_graph
		{
			int band_index;
			int freq;
			int gain_values;
			AVFilterContext* eq_context;
			std::string eq_name;
		};
		AVFilterContext* filter_context_src = nullptr, * filter_context_sink = nullptr, * resample_ctx = nullptr,
			* volume_ctx = nullptr, * limiter_ctx = nullptr, * format_normalize_ctx = nullptr;
		std::vector<av_filter_eq_graph> filter_graphs;

		int init_av_filter_equalizer();
		bool is_av_filter_equalizer_initialized();
		void reset_av_filter_equalizer();
	public:
		volatile bool suppress_time_events = false;

		// constructor
		MusicPlayerNative();
		// no copy & move
		MusicPlayerNative(const MusicPlayerNative&) = delete;
		MusicPlayerNative& operator=(const MusicPlayerNative&) = delete;
		MusicPlayerNative(MusicPlayerNative&&) = delete;
		MusicPlayerNative& operator=(MusicPlayerNative&&) = delete;

		// Interfaces
		bool IsInitialized();
		bool IsPlaying();
		void OpenFile(const std::wstring& fileName, const std::wstring& file_extension_in = {}, bool skip_album_art_loading = false);
		double GetMusicTimeLength();
		double GetCurrentMusicPosition();
		std::wstring GetSongTitle();
		std::wstring GetSongArtist();
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

		// Managed C++ Interface
		void SetManagedPlayer(MusicPlayer^);

		// destructor
		~MusicPlayerNative();
	};

	public delegate void PlayerFileInitDelegate();
	public delegate void PlayerAlbumArtInitDelegate(array<System::Byte>^ encodedImage);
	public delegate void PlayerStartDelegate();
	public delegate void PlayerPauseDelegate();
	public delegate void PlayerStopDelegate();
	public delegate void PlayerTimeChangeDelegate(double time);
	public delegate void PlayerDestroyDelegate();
	public delegate void PlayerErrorDelegate(System::Exception^ exception);
	public delegate void PlayerNcmRequireAlbumArtDownloadDelegate(String^ url);

	public ref class MusicPlayer:
		System::ICloneable, System::IDisposable
	{
		MusicPlayerNative* native_handle;

	public:
		property PlayerFileInitDelegate^ OnPlayerFileInit;
		property PlayerAlbumArtInitDelegate^ OnPlayerAlbumArtInit;
		property PlayerStartDelegate^ OnPlayerStart;
		property PlayerPauseDelegate^ OnPlayerPause;
		property PlayerStopDelegate^ OnPlayerStop;
		property PlayerTimeChangeDelegate^ OnPlayerTimeChange;
		property PlayerDestroyDelegate^ OnPlayerDestroy;
		property PlayerErrorDelegate^ OnPlayerError;
		property PlayerNcmRequireAlbumArtDownloadDelegate^ OnPlayerNcmRequireAlbumArtDownload;
		
		MusicPlayer();
		MusicPlayer(int sample_rate);

	private:
		void check_if_null();
		bool is_native_valid() { return native_handle != nullptr; }

		ref class ProcessEventState {
		public:
			MessageType EventType;
			Object^ Payload;
		};
		void ProcessEventCore(Object^ state);

	public:

		/*
		* ProcessEvent is called by native code to notify managed code of various events, such as file initialization, album art initialization, playback start/pause/stop, and time change.
		* Notice: this function is dispatched asynchronously via ThreadPool to avoid deadlocks between native audio threads and the UI/managed thread.
		* Callback functions should NOT perform any heavy operation to avoid blocking the audio thread, which may cause audio stutter.
		* Invoke or similar mechanism to avoid cross-thread operation exceptions.
		*/
		void ProcessEvent(MessageType event_type, Object^ payload);
		void ProcessAlbumArtEvent(array<System::Byte>^ encodedImage);
		void ProcessError(System::Exception^ exception);

		bool IsInitialized();
		bool IsPlaying();
		void OpenFile(const System::String^ fileName);
		void OpenFile(const System::String^ fileName, bool skipAlbumArtLoading);
		double GetMusicTimeLength();
		double GetCurrentMusicPosition();
		System::String^ GetSongTitle();
		System::String^ GetSongArtist();
		void Start();
		void Pause();
		void Stop();
		void SetMasterVolume(float volume);
		void SeekToPosition(double time, bool need_stop);
		// int GetRawPCMBytes(uint8_t* buffer_out, int buffer_size) const;

		int GetNBlockAlign();
		System::String^ GetID3Lyric();

		// FFT spectrum data
		int CopyAudioFFTData(array<float>^ destination);

		// Equalizer interfaces
		int GetEqualizerBand(int index);
		void SetEqualizerBand(int index, int value);

		virtual Object^ Clone() {
			throw gcnew System::NotSupportedException("This object cannot be cloned.");
		}

		~MusicPlayer();

		!MusicPlayer();
	};

	public ref class SmtcInteropHelper abstract sealed
	{
	public:
		static IntPtr GetSmtcForWindow(IntPtr hWnd);
	};

}
