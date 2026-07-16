// SPDX-License-Identifier: MIT

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#define kiss_fft_scalar double
#include <kissfft/kiss_fft.h>

#include "Audio/AudioOutputFormat.h"

struct SwrContext;

namespace MusicPlayerLibrary
{
	struct AudioFrameData {
		std::vector<uint8_t> data;
		int samples;
	};
	
	// for fast-fourier transform execution
	// 当你觉得自己没用的时候，你可以想想某个瞎改了一晚上把频谱图改成馒头然后回滚的我
	class FFTExecuter
	{
		static constexpr int FFT_SAMPLE_RATE = 48000;
		static constexpr size_t BYTES_PER_FRAME = 4; // S16 * Stereo
		static constexpr size_t FFT_SIZE = 2048;     // fixed fft size
		static constexpr size_t RING_BUFFER_MAX_SIZE = FFT_SIZE * BYTES_PER_FRAME;
		static constexpr int FFT_FRAME_INTERVAL_MS = 16;
	public:
		void AddSamplesToRingBuffer(
			const uint8_t* interleaved_samples,
			int frame_count);
		[[nodiscard]] std::vector<uint8_t> ResampleToFftFormat(
			const uint8_t* interleaved_samples,
			int frame_count);
		[[nodiscard]] int GetRingBufferSize() const;
		// XAudio2 output latency compensation.
		void SetOutputDelayMilliseconds(int milliseconds);
	protected:
		// apply window to ring buffer, convert to vector
		void ApplyWindow(const std::vector<uint8_t>& input, std::vector<double>& output);
		void DoFFT(const std::vector<double>& windowed_data, std::vector<float>&, kiss_fft_cfg fft_cfg);
		std::vector<size_t> GenBoundaries(float sample_rate, size_t fft_size, size_t segment_num, float f_lo = 20.0f, float f_hi = 20000.0f);
		void MapFreqToSegments(std::vector<float>&, std::vector<float>&, const std::vector<size_t>&);

	public:
		void ExecuteAudioFFT();
		int CopyAudioFFTData(float* destination, int destination_length);
		void ResetBuffers();
		void StartFFTThread();
		void StopFFTThread();
		void FFTWorkerLoop();

		explicit FFTExecuter(const AudioOutputFormat& output_format);
		~FFTExecuter();

	protected:
		// ring buffer
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
		std::deque<std::vector<float>> spectrum_delay_queue;
		std::atomic<int> delay_frames{0};
		static constexpr size_t MAX_DELAY_QUEUE_SIZE = 128;

		// avoid duplicate allocation
		kiss_fft_cfg fft_cfg = nullptr;
		static constexpr size_t fft_size = FFT_SIZE;
		static constexpr int sample_rate = FFT_SAMPLE_RATE;
		std::vector<kiss_fft_cpx> fft_in;
		std::vector<kiss_fft_cpx> fft_out;

		
		// fixed FFT format: 48000/S16/Stereo
		SwrContext* resample_context = nullptr;
		mutable std::mutex resample_mutex;
	};

}
