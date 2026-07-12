// SPDX-License-Identifier: MIT

#pragma once
#include <mutex>
#include <deque>

#define kiss_fft_scalar double
#include <kissfft/kiss_fft.h>

namespace MusicPlayerLibrary
{
	struct AudioFrameData {
		std::vector<uint8_t> data;
		int samples;
	};
	
	// for fast-fourier transform execution
	class FFTExecuter
	{
		static constexpr size_t BYTES_PER_FRAME = 4; // 16bit * 2ch
		static constexpr int BASE_SAMPLE_RATE = 48000;
		static constexpr size_t BASE_FFT_SIZE = 2048;
		static constexpr size_t MIN_FFT_SIZE = 512;
		static constexpr size_t MAX_FFT_SIZE = 8192;
		static constexpr int FFT_FRAME_INTERVAL_MS = 16;
	public:
		void AddSamplesToRingBuffer(const uint8_t* samples, int sample_size);
		[[nodiscard]] int GetRingBufferSize() const;
		// XAudio2 output latency compensation.
		void SetOutputDelayMilliseconds(int milliseconds);
	protected:
		struct SpectrumDelayFrame
		{
			std::vector<float> data;
			std::chrono::steady_clock::time_point captured_at;
		};

		static size_t SelectFftSize(int sample_rate);
		[[nodiscard]] size_t GetRingBufferMaxSize() const;
		// apply window to ring buffer, convert to vector
		void ApplyWindow(const std::vector<uint8_t>& input, std::vector<double>& output);
		void DoFFT(const std::vector<double>& windowed_data, std::vector<float>&, kiss_fft_cfg fft_cfg);
		std::vector<size_t> GenBoundaries(float sample_rate, size_t fft_size, size_t segment_num, float f_lo = 20.0f, float f_hi = 20000.0f);
		void MapFreqToSegments(const std::vector<float>&, std::vector<float>&, const std::vector<size_t>&);

	public:
		void ExecuteAudioFFT();
		int CopyAudioFFTData(float* destination, int destination_length);
		void ResetBuffers();
		void StartFFTThread();
		void StopFFTThread();
		void FFTWorkerLoop();

		FFTExecuter(int sample_rate);
		~FFTExecuter();

	protected:
		// ring buffer, sized from the per-sample-rate FFT size
		std::deque<uint8_t> spectrum_data_ring_buffer;
		std::vector<float> spectrum_data;
		std::vector<float> spectrum_max_data{};
		std::vector<float> spectrum_smooth_data{};
		mutable std::mutex ring_buffer_mutex;
		mutable std::mutex spectrum_data_mutex;
		std::condition_variable ring_buffer_cv;
		bool ring_buffer_has_unprocessed_data = false;
		std::thread fft_worker_thread;
		std::atomic<bool> fft_thread_running{ false };

		// delay queue for latency compensation
		std::deque<SpectrumDelayFrame> spectrum_delay_queue;
		std::atomic<int> delay_ms{0};
		static constexpr size_t MAX_DELAY_QUEUE_SIZE = 128;

		// avoid duplicate allocation
		kiss_fft_cfg fft_cfg = nullptr;
		size_t fft_size = BASE_FFT_SIZE;
		int sample_rate = 0;
		std::vector<kiss_fft_cpx> fft_in;
		std::vector<kiss_fft_cpx> fft_out;
	};

}
