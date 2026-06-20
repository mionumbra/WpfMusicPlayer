#pragma once

#include "AtlTraceRedirect.h"
#include <atomic>
#include <vcclr.h>
#include "FFTExecuter.h"
#include "FileAbstractionLayer.h"
#include "StringUtils.h"
using namespace System;

namespace MusicPlayerLibrary {

	public ref class AtlTraceRedirectManager {
		static AtlTraceRedirect* m_pRedirector;
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

	public enum MessageType : UINT {
		WM_PLAYER_FILE_INIT = (WM_USER + 100),
		WM_PLAYER_TIME_CHANGE = (WM_USER + 101),
		WM_PLAYER_START = (WM_USER + 102),
		WM_PLAYER_PAUSE = (WM_USER + 103),
		WM_PLAYER_STOP = (WM_USER + 104),
		WM_PLAYER_ALBUM_ART_INIT = (WM_USER + 105),
		WM_PLAYER_DESTROY = (WM_USER + 106),
		WM_PLAYER_ERROR = (WM_USER + 107)
	};

	public struct NcmMusicMeta
	{
		std::wstring musicName;
		std::vector<std::vector<std::wstring>> artist;
		std::wstring format;
		std::wstring album;
		std::wstring albumPic;
	};

	public struct DecryptResult
	{
		std::wstring title;
		std::wstring artist;
		std::wstring album;
		std::wstring ext;
		std::wstring pictureUrl;
		std::wstring mime;
	};

	public class NcmDecryptor
	{
	public:
		NcmDecryptor(IFile& source_file, const std::wstring& filename);
		DecryptResult Decrypt(IFile& output_file);

	private:
		IFile& m_sourceFile;
		ULONGLONG m_fileLength = 0;
		ULONGLONG m_offset = 0;
		std::wstring m_filename;

		NcmMusicMeta m_oriMeta;
		std::wstring m_format;
		std::wstring m_mime;

		void EnsureSourceRange(ULONGLONG offset, ULONGLONG count, const char* message) const;
		void SeekSource(ULONGLONG offset, const char* message);
		void ReadSourceExact(void* buffer, UINT count, const char* message);
		uint32_t ReadSourceUint32(const char* message);
		uint32_t ReadSourceUint32At(ULONGLONG offset, const char* message);
		std::vector<uint8_t> ReadSourceBytes(UINT count, const char* message);
		std::vector<uint8_t> GetKeyData();
		std::vector<uint8_t> GetKeyBox();
		NcmMusicMeta GetMetaData();
		void WriteAudio(const std::vector<uint8_t>& keyBox, IFile& output_file);
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
		std::unique_ptr<IFile> file_stream;
		std::atomic_bool file_stream_end = false;
		std::atomic_bool user_request_stop = false;
		std::atomic<double> pts_seconds = 0.0;
		std::atomic<float> elapsed_time = 0.0;
		std::atomic<float> length = 0.0f;
		std::atomic_bool is_pause = false;
		std::atomic_bool decoder_is_running = false;
		int fifo_audio_channels = 0;
		AVSampleFormat fifo_audio_sample_fmt = AV_SAMPLE_FMT_NONE;
		int fifo_sample_rate = 0;
		HBITMAP album_art = nullptr;
		std::wstring song_title = {};
		std::wstring song_artist = {};

		std::mutex audio_fifo_mutex;
		std::mutex audio_playback_mutex;
		std::mutex frame_event_mutex;
		std::condition_variable frame_ready_cv;
		std::condition_variable frame_underrun_cv;
		bool frame_ready_requested = false;
		bool frame_underrun_requested = false;

		IXAudio2* xaudio2 = nullptr;
		IXAudio2MasteringVoice* mastering_voice = nullptr;
		IXAudio2SourceVoice* source_voice = nullptr;
		WAVEFORMATEX wfx = {};

		std::atomic_int playback_state;
		std::jthread audio_player_worker_thread;
		std::jthread audio_decoder_worker_thread;
		std::jthread album_art_worker_thread;

		std::list<XAUDIO2_BUFFER*> xaudio2_playing_buffers = {};
		std::list<XAUDIO2_BUFFER*> xaudio2_free_buffers = {};
		size_t xaudio2_played_buffers = 0, xaudio2_allocated_buffers = 0, xaudio2_played_samples = 0;
		size_t base_offset = 0;

		// use avaudiofifo to avoid lag on low-cpu performance system, like jasper lake/alder lake-n
		AVAudioFifo* audio_fifo = nullptr;
		int xaudio2_play_frame_size = 256;
		LPDWORD xaudio2_thread_task_index = nullptr;
		double standard_frametime = 0.0, last_frametime = 0.0;
		float message_interval = 16.67f, message_interval_timer = 0.0f;
		size_t prev_decode_cycle_xaudio2_played_samples = 0;
		std::wstring id3_string_lyric;
		int sample_rate = 48000;

		// managed variables
		gcroot<MusicPlayer^> managed_music_player;

		// file I/O Area
		int read_func(uint8_t* buf, int buf_size);
		static int read_func_wrapper(void* opaque, uint8_t* buf, int buf_size);
		int64_t seek_func(int64_t offset, int whence);
		static int64_t seek_func_wrapper(void* opaque, int64_t offset, int whence);
		int load_audio_context(const std::wstring& audio_filename, const std::wstring& file_extension_in = {});
		int load_audio_context_from_file_stream();
		void release_audio_context();
		void reset_audio_context();
		bool is_audio_context_initialized();
		static HBITMAP download_ncm_album_art(const std::wstring& url, int scale_size = 128);
		HBITMAP decode_id3_album_art(int stream_index, int scale_size = 128);
		void download_ncm_album_art_async(const std::wstring& url, int scale_size);
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
		void handle_worker_exception(System::Exception^ exception, const char* worker_name);
		void start_audio_playback();
		void stop_audio_decode(int mode = 0);
		void stop_audio_playback(int mode);

		int initialize_audio_fifo(AVSampleFormat sample_fmt, int channels, int nb_samples);
		int resize_audio_fifo(int nb_samples);
		int add_samples_to_fifo(uint8_t** decoded_data, int nb_samples);
		int read_samples_from_fifo(uint8_t** output_buffer, int nb_samples);
		void drain_audio_fifo(int nb_samples);
		void reset_audio_fifo();
		int get_audio_fifo_cached_samples_size();
		void uninitialize_audio_fifo();

		// XAudio2 helper function
		const char* get_backend_implement_version();
		void xaudio2_init_buffer(XAUDIO2_BUFFER* dest_buffer, int size = 8192);
		XAUDIO2_BUFFER* xaudio2_allocate_buffer(int size = 8192);
		XAUDIO2_BUFFER* xaudio2_get_available_buffer(int size = 8192);
		void xaudio2_free_buffer();
		void xaudio2_destroy_buffer();
		int decoder_query_xaudio2_buffer_size();
		bool is_xaudio2_initialized();
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
		void OpenFile(const std::wstring& fileName, const std::wstring& file_extension_in = {});
		float GetMusicTimeLength();
		float GetCurrentMusicPosition();
		std::wstring GetSongTitle();
		std::wstring GetSongArtist();
		void Start();
		void Pause();
		void Stop();
		void SetMasterVolume(float volume);
		void SeekToPosition(float time, bool need_stop);
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
	public delegate void PlayerAlbumArtInitDelegate(System::Drawing::Image^ fromDecode);
	public delegate void PlayerStartDelegate();
	public delegate void PlayerPauseDelegate();
	public delegate void PlayerStopDelegate();
	public delegate void PlayerTimeChangeDelegate(float time);
	public delegate void PlayerDestroyDelegate();
	public delegate void PlayerErrorDelegate(System::Exception^ exception);

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
	public:
		MusicPlayer();
		MusicPlayer(int sample_rate);

	private:
		void check_if_null();
		bool is_native_valid() { return native_handle != nullptr; }

		ref class ProcessEventState {
		public:
			MessageType EventType;
			IntPtr WParam;
			IntPtr LParam;
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
		void ProcessEvent(MessageType event_type, WPARAM wParam, LPARAM lParam);
		void ProcessError(System::Exception^ exception);

		bool IsInitialized();
		bool IsPlaying();
		void OpenFile(const System::String^ fileName);
		float GetMusicTimeLength();
		float GetCurrentMusicPosition();
		System::String^ GetSongTitle();
		System::String^ GetSongArtist();
		void Start();
		void Pause();
		void Stop();
		void SetMasterVolume(float volume);
		void SeekToPosition(float time, bool need_stop);
		// int GetRawPCMBytes(uint8_t* buffer_out, int buffer_size) const;

		int GetNBlockAlign();
		System::String^ GetID3Lyric();

		// FFT spectrum data
		array<float>^ GetAudioFFTData();

		// Equalizer interfaces
		int GetEqualizerBand(int index);
		void SetEqualizerBand(int index, int value);

		virtual Object^ Clone() {
			throw gcnew System::NotSupportedException("This object cannot be cloned.");
		}

		~MusicPlayer() {
			delete native_handle;
			native_handle = nullptr;
			OnPlayerFileInit = nullptr;
			OnPlayerAlbumArtInit = nullptr;
			OnPlayerStart = nullptr;
			OnPlayerPause = nullptr;
			OnPlayerStop = nullptr;
			OnPlayerTimeChange = nullptr;
			OnPlayerDestroy = nullptr;
			OnPlayerError = nullptr;
		}
	};

	public ref class SmtcInteropHelper abstract sealed
	{
	public:
		static IntPtr GetSmtcForWindow(IntPtr hWnd);
	};

}
