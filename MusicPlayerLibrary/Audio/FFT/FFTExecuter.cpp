// SPDX-License-Identifier: MIT

#include "pch.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

#include "Audio/FFT/FFTExecuter.h"

#if defined(__cplusplus)
extern "C" {
#endif
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libswresample/swresample.h>
#if defined(__cplusplus)
}
#endif

namespace
{
	std::string FfmpegErrorMessage(const int error_code)
	{
		char message[AV_ERROR_MAX_STRING_SIZE]{};
		av_strerror(error_code, message, sizeof(message));
		return message;
	}
}

std::vector<uint8_t> MusicPlayerLibrary::FFTExecuter::ResampleToFftFormat(
	const uint8_t* interleaved_samples,
	const int frame_count)
{
	if (!interleaved_samples || frame_count <= 0)
		return {};

	std::lock_guard resample_lock(resample_mutex);
	if (!resample_context)
		return {};

	const int output_capacity = swr_get_out_samples(resample_context, frame_count);
	if (output_capacity < 0)
	{
		NATIVE_TRACE("err: query FFT resample output size failed, reason=%s\n",
			FfmpegErrorMessage(output_capacity).c_str());
		return {};
	}
	if (output_capacity == 0)
		return {};

	std::vector<uint8_t> result(
		static_cast<size_t>(output_capacity) * BYTES_PER_FRAME);
	const uint8_t* input_planes[] = { interleaved_samples };
	uint8_t* output_planes[] = { result.data() };
	const int converted_frames = swr_convert(
		resample_context,
		output_planes,
		output_capacity,
		input_planes,
		frame_count);
	if (converted_frames < 0)
	{
		NATIVE_TRACE("err: FFT resample failed, reason=%s\n",
			FfmpegErrorMessage(converted_frames).c_str());
		return {};
	}

	result.resize(static_cast<size_t>(converted_frames) * BYTES_PER_FRAME);
	return result;
}

void MusicPlayerLibrary::FFTExecuter::AddSamplesToRingBuffer(
	const uint8_t* interleaved_samples,
	const int frame_count)
{
	std::vector<uint8_t> samples =
		ResampleToFftFormat(interleaved_samples, frame_count);
	if (samples.empty())
		return;

	std::lock_guard ring_buffer_lock(ring_buffer_mutex);
	for (const uint8_t sample : samples)
	{
		spectrum_data_ring_buffer.push_back(sample);
	}

	// ReSharper disable once CppDFALoopConditionNotUpdated
	while (spectrum_data_ring_buffer.size() > RING_BUFFER_MAX_SIZE)
	{
		spectrum_data_ring_buffer.pop_front();
	}
	ring_buffer_has_unprocessed_data = true;

	// wake fft consumer thread
	ring_buffer_cv.notify_one();
}

int MusicPlayerLibrary::FFTExecuter::GetRingBufferSize() const
{
	std::lock_guard lock(ring_buffer_mutex);
	return static_cast<int>(spectrum_data_ring_buffer.size());
}

void MusicPlayerLibrary::FFTExecuter::SetOutputDelayMilliseconds(int milliseconds)
{
	delay_frames.store(milliseconds > 0 ? milliseconds / FFT_FRAME_INTERVAL_MS : 0);
}

void MusicPlayerLibrary::FFTExecuter::ApplyWindow(const std::vector<uint8_t>& input, std::vector<double>& output)
{
	const size_t bytes_per_frame = 4;  // 2 channels * 2 bytes
	const size_t frame_count = input.size() / bytes_per_frame;
	output.resize(frame_count);

	for (size_t i = 0; i < frame_count; ++i) {
		auto left = static_cast<int16_t>(input[i * 4] | (input[i * 4 + 1] << 8));
		auto right = static_cast<int16_t>(input[i * 4 + 2] | (input[i * 4 + 3] << 8));
		// mix 2 channels
		double sample = (static_cast<double>(left) + static_cast<double>(right)) / 2.0;
		// hamming window
		const double w = 0.53836 * (1.0 - cos(2.0 * std::numbers::pi * i / (frame_count - 1)));
		// normalize
		output[i] = (sample / 32768.0) * w;
	}
}

void MusicPlayerLibrary::FFTExecuter::DoFFT(const std::vector<double>& windowed_data, std::vector<float>& fft_result, kiss_fft_cfg fft_cfg)
{
	size_t n = windowed_data.size();
	if (n == 0) return;

	// padding imaginary part to zero
	// cause kissfft only supports real input
	for (size_t i = 0; i < n; ++i) {
		fft_in[i].r = windowed_data[i];
		fft_in[i].i = 0.0f;
	}
	// zero padding
	for (size_t i = n; i < fft_size; ++i) {
		fft_in[i].r = 0.0;
		fft_in[i].i = 0.0;
	}

	kiss_fft(fft_cfg, fft_in.data(), fft_out.data());

	// Magnitude spectrum.
	fft_result.resize(fft_size / 2);
	for (size_t i = 0; i < fft_size / 2; ++i) {
		float real = fft_out[i].r;
		float imag = fft_out[i].i;
		fft_result[i] = sqrtf(real * real + imag * imag);
	}
}

std::vector<size_t> MusicPlayerLibrary::FFTExecuter::GenBoundaries(float sample_rate, size_t fft_size, size_t segment_num, float f_lo, float f_hi)
{
	std::vector<size_t> boundaries(segment_num + 1);
	float delta_f = sample_rate / fft_size;
	size_t max_bin = fft_size / 2; // Use only the positive-frequency half.

	for (size_t i = 0; i <= segment_num; ++i) {
		float fraction = static_cast<float>(i) / segment_num;
		float freq = f_lo * pow(f_hi / f_lo, fraction);        // Logarithmic interpolation.
		auto idx = static_cast<size_t>(freq / delta_f);
		if (idx > max_bin - 1) idx = max_bin - 1;
		boundaries[i] = idx;
	}
	// Remove duplicates so that every segment contains at least one bin.
	for (size_t i = 1; i < boundaries.size(); ++i)
		if (boundaries[i] <= boundaries[i - 1]) boundaries[i] = boundaries[i - 1] + 1;
	if (boundaries.back() > max_bin) boundaries.back() = max_bin;
	return boundaries;
}

void MusicPlayerLibrary::FFTExecuter::MapFreqToSegments(
	std::vector<float>& fft_result,
	std::vector<float>& segments,
	const std::vector<size_t>& bandBounds)
{
	size_t numSegments = bandBounds.size() - 1;
	segments.resize(numSegments);
	for (size_t i = 0; i < numSegments; ++i) {
		float maxVal = 0.0f;
		for (size_t j = bandBounds[i]; j < bandBounds[i + 1]; ++j) {
			if (j >= fft_result.size()) break;
			if (fft_result[j] > maxVal) maxVal = fft_result[j];
		}
		segments[i] = maxVal;
	}
}

void MusicPlayerLibrary::FFTExecuter::ExecuteAudioFFT()
{

	std::vector<uint8_t> raw_samples;
	{
		std::lock_guard lock(ring_buffer_mutex);
		// Check whether the buffer contains a complete FFT frame.
		if (spectrum_data_ring_buffer.size() < RING_BUFFER_MAX_SIZE || !ring_buffer_has_unprocessed_data)
			return;

		// drain data
		raw_samples.assign(
			spectrum_data_ring_buffer.begin(),
			spectrum_data_ring_buffer.begin() + RING_BUFFER_MAX_SIZE);
		ring_buffer_has_unprocessed_data = false;
	}

	// windowing
	std::vector<double> windowed;
	ApplyWindow(raw_samples, windowed);
	if (windowed.empty())
		return;

	// FFT
	std::vector<float> fft_result;
	DoFFT(windowed, fft_result, fft_cfg);

	if (fft_result.empty())
		return;

	// customizable sample rate, 32 segments
	constexpr size_t segment_num = 32;

	auto boundaries = GenBoundaries(sample_rate, fft_size, segment_num);

	{
		std::lock_guard<std::mutex> lock(spectrum_data_mutex); spectrum_data.clear();
		MapFreqToSegments(fft_result, spectrum_data, boundaries);

		for (size_t i = 0; i < spectrum_data.size(); ++i) {
			float& val = spectrum_data[i];
			// transition db
			float db = 20.0f * log10f(val + 1e-6f);
			constexpr float db_min = 10.0f;   // supress noise
			constexpr float db_max = 45.0f;   // full
			val = (db - db_min) / (db_max - db_min);
			if (val < 0.0f) val = 0.0f;
			if (val > 1.0f) val = 1.0f;

			// high freq attenuation
			constexpr size_t high_freq_start = segment_num * 2 / 3;
			if (i >= high_freq_start) {
				float attenuation = 1.0f - 0.4f * static_cast<float>(i - high_freq_start) / (segment_num - high_freq_start);
				val *= attenuation;
			}
		}

		// Push the computed data to the delay queue for latency compensation.
		spectrum_delay_queue.push_back(spectrum_data);
		while (spectrum_delay_queue.size() > MAX_DELAY_QUEUE_SIZE)
			spectrum_delay_queue.pop_front();
	}

}

int MusicPlayerLibrary::FFTExecuter::CopyAudioFFTData(float* destination, int destination_length)
{
	if (destination == nullptr || destination_length <= 0)
		return 0;

	std::lock_guard lock(spectrum_data_mutex);
	if (spectrum_delay_queue.empty())
		return 0;

	int delay = delay_frames.load();
	int queue_size = static_cast<int>(spectrum_delay_queue.size());
	// Calculate the frame index from the configured delay.
	int target = queue_size - 1 - delay;
	if (target < 0) target = 0;

	const auto& data = spectrum_delay_queue[target];
	const int copy_count = std::min(destination_length, static_cast<int>(data.size()));
	if (copy_count > 0)
		std::copy_n(data.data(), copy_count, destination);
	return copy_count;
}

void MusicPlayerLibrary::FFTExecuter::ResetBuffers()
{
	{
		std::lock_guard resample_lock(resample_mutex);
		if (resample_context)
		{
			swr_close(resample_context);
			if (const int result = swr_init(resample_context); result < 0)
			{
				NATIVE_TRACE("err: reset FFT resample context failed, reason=%s\n",
					FfmpegErrorMessage(result).c_str());
				swr_free(&resample_context);
			}
		}
	}

	{
		std::lock_guard lock(ring_buffer_mutex);
		spectrum_data_ring_buffer.clear();
		ring_buffer_has_unprocessed_data = false;
	}

	{
		std::lock_guard lock(spectrum_data_mutex);
		spectrum_data.clear();
		spectrum_smooth_data.clear();
		spectrum_delay_queue.clear();
	}
}

void MusicPlayerLibrary::FFTExecuter::StartFFTThread()
{
	if (fft_thread_running.exchange(true))
		return;

	fft_worker_thread = std::thread(&FFTExecuter::FFTWorkerLoop, this);
}

void MusicPlayerLibrary::FFTExecuter::StopFFTThread()
{
	if (!fft_thread_running.exchange(false))
		return;

	ring_buffer_cv.notify_all();
	if (fft_worker_thread.joinable())
		fft_worker_thread.join();

	spectrum_data.clear();
	spectrum_smooth_data.clear();
	spectrum_delay_queue.clear();
	spectrum_data_ring_buffer.clear();
	fft_in.clear();
	fft_out.clear();
}

void MusicPlayerLibrary::FFTExecuter::FFTWorkerLoop()
{
	auto next_fft_time = std::chrono::steady_clock::now();

	while (fft_thread_running)
	{
		std::unique_lock lock(ring_buffer_mutex);
		// FFT execution limitd to 60fps
		ring_buffer_cv.wait(lock, [this]()
			{
				return !fft_thread_running ||
					(ring_buffer_has_unprocessed_data &&
						spectrum_data_ring_buffer.size() >= RING_BUFFER_MAX_SIZE);
			});

		if (!fft_thread_running)
			break;

		const auto now = std::chrono::steady_clock::now();
		if (now < next_fft_time)
		{
			ring_buffer_cv.wait_until(lock, next_fft_time, [this]()
				{
					return !fft_thread_running;
				});

			if (!fft_thread_running)
				break;

			if (!ring_buffer_has_unprocessed_data ||
				spectrum_data_ring_buffer.size() < RING_BUFFER_MAX_SIZE)
				continue;
		}

		lock.unlock();
		ExecuteAudioFFT();
		next_fft_time = std::chrono::steady_clock::now() + std::chrono::milliseconds(FFT_FRAME_INTERVAL_MS);
	}
}

MusicPlayerLibrary::FFTExecuter::FFTExecuter(const AudioOutputFormat& output_format)
{
	if (output_format.sample_rate <= 0 || output_format.channel_count == 0 ||
		output_format.sample_format == AV_SAMPLE_FMT_NONE ||
		av_sample_fmt_is_planar(output_format.sample_format))
	{
		throw std::invalid_argument("FFTExecuter requires a valid packed audio output format");
	}

	AVChannelLayout input_layout{};
	int result;
	if (output_format.channel_mask != 0)
		result = av_channel_layout_from_mask(&input_layout, output_format.channel_mask);
	else
	{
		av_channel_layout_default(&input_layout, output_format.channel_count);
		result = input_layout.nb_channels > 0 ? 0 : AVERROR_INVALIDDATA;
	}
	if (result < 0 || input_layout.nb_channels != output_format.channel_count)
	{
		av_channel_layout_uninit(&input_layout);
		throw std::invalid_argument("FFTExecuter input channel layout does not match channel count");
	}

	AVChannelLayout fft_layout = AV_CHANNEL_LAYOUT_STEREO;
	SwrContext* local_resample_context = nullptr;
	result = swr_alloc_set_opts2(
		&local_resample_context,
		&fft_layout,
		AV_SAMPLE_FMT_S16,
		FFT_SAMPLE_RATE,
		&input_layout,
		output_format.sample_format,
		output_format.sample_rate,
		0,
		nullptr);
	av_channel_layout_uninit(&fft_layout);
	av_channel_layout_uninit(&input_layout);
	if (result < 0 || !local_resample_context)
	{
		throw std::runtime_error(
			"swr_alloc_set_opts2 failed: " + FfmpegErrorMessage(result));
	}
	result = swr_init(local_resample_context);
	if (result < 0)
	{
		swr_free(&local_resample_context);
		throw std::runtime_error("swr_init failed: " + FfmpegErrorMessage(result));
	}

	fft_cfg = kiss_fft_alloc(fft_size, 0, nullptr, nullptr);
	if (!fft_cfg)
	{
		swr_free(&local_resample_context);
		throw std::runtime_error("kiss_fft_alloc failed!");
	}
	resample_context = local_resample_context;

	fft_in.resize(fft_size);
	fft_out.resize(fft_size);
	StartFFTThread();
}

MusicPlayerLibrary::FFTExecuter::~FFTExecuter()
{
	StopFFTThread();
	{
		std::lock_guard resample_lock(resample_mutex);
		swr_free(&resample_context);
	}
	if (fft_cfg)
	{
		kiss_fft_free(fft_cfg);
		fft_cfg = nullptr;
	}
}
