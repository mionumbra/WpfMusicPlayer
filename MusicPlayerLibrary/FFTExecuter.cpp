// SPDX-License-Identifier: MIT

#include "pch.h"
#include "FFTExecuter.h"

size_t MusicPlayerLibrary::FFTExecuter::SelectFftSize(int sample_rate)
{
    if (sample_rate <= 0)
        return BASE_FFT_SIZE;

    // 根据采样率，动态缩放FFT采样大小
    const double target_size =
        static_cast<double>(BASE_FFT_SIZE) * sample_rate / BASE_SAMPLE_RATE;

    size_t selected_size = MIN_FFT_SIZE;
    while (selected_size < MAX_FFT_SIZE && selected_size < target_size)
        selected_size <<= 1;

    return selected_size;
}

size_t MusicPlayerLibrary::FFTExecuter::GetRingBufferMaxSize() const
{
    return fft_size * BYTES_PER_FRAME;
}

void MusicPlayerLibrary::FFTExecuter::AddSamplesToRingBuffer(uint8_t* samples, int sample_size)
{
    std::lock_guard ring_buffer_lock(ring_buffer_mutex);
    if (samples == nullptr || sample_size <= 0)
        return;

    for (int i = 0; i < sample_size; ++i)
    {
        spectrum_data_ring_buffer.push_back(samples[i]);
    }

    const size_t ring_buffer_max_size = GetRingBufferMaxSize();
    // ReSharper disable once CppDFALoopConditionNotUpdated
    while (spectrum_data_ring_buffer.size() > ring_buffer_max_size)
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
    delay_ms.store(milliseconds > 0 ? milliseconds : 0);
}

void MusicPlayerLibrary::FFTExecuter::ApplyWindow(const std::vector<uint8_t>& input, std::vector<double>& output)
{
    const size_t frame_count = input.size() / BYTES_PER_FRAME;
    output.resize(frame_count);

    for (size_t i = 0; i < frame_count; ++i) {
        auto left = static_cast<int16_t>(input[i * 4] | (input[i * 4 + 1] << 8));
        auto right = static_cast<int16_t>(input[i * 4 + 2] | (input[i * 4 + 3] << 8));
        // mix 2 channels
        double sample = (static_cast<double>(left) + static_cast<double>(right)) / 2.0;
        // hamming window
        const double w = 0.53836 * (1.0 - cos(2.0 * M_PI * i / (frame_count - 1)));
        // normalize
        output[i] = (sample / 32768.0) * w;
    }
}

void MusicPlayerLibrary::FFTExecuter::DoFFT(const std::vector<double>& windowed_data, std::vector<float>& fft_result, kiss_fft_cfg fft_cfg)
{
    const size_t n = (std::min)(windowed_data.size(), fft_size);
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
        const double real = fft_out[i].r;
        const double imag = fft_out[i].i;
        fft_result[i] = static_cast<float>(std::sqrt(real * real + imag * imag));
    }
}

std::vector<size_t> MusicPlayerLibrary::FFTExecuter::GenBoundaries(float sample_rate, size_t fft_size, size_t segment_num, float f_lo, float f_hi)
{
    std::vector<size_t> boundaries(segment_num + 1);
    if (sample_rate <= 0 || fft_size < 2 || segment_num == 0)
        return boundaries;

    const double delta_f = static_cast<double>(sample_rate) / fft_size;
    const size_t max_bin = fft_size / 2; // 取采样率/2为音频文件有效频率范围（奈奎斯特采样定律）
    if (max_bin == 0)
        return boundaries;

    const double nyquist = static_cast<double>(sample_rate) / 2.0;
    const double lo = static_cast<double>(f_lo) < 1.0 ? 1.0 : static_cast<double>(f_lo);
    const double requested_hi = static_cast<double>(f_hi) > nyquist ? nyquist : static_cast<double>(f_hi);
    double upper_freq = requested_hi > lo + delta_f ? requested_hi : lo + delta_f;
    if (upper_freq > nyquist)
        upper_freq = nyquist;

    size_t first_bin = static_cast<size_t>(std::ceil(lo / delta_f));
    if (first_bin < 1)
        first_bin = 1;
    if (first_bin >= max_bin)
        first_bin = max_bin - 1;

    boundaries[0] = first_bin;
    for (size_t i = 1; i <= segment_num; ++i) {
        const double fraction = static_cast<double>(i) / segment_num;
        const double freq = lo * std::pow(upper_freq / lo, fraction); // 对数插值
        size_t boundary_index = static_cast<size_t>(std::ceil(freq / delta_f));
        if (boundary_index > max_bin)
            boundary_index = max_bin;

        // keep every visible segment non-empty without crossing nyquist
        const size_t min_idx = boundaries[i - 1] + 1;
        const size_t remaining_segments = segment_num - i;
        size_t max_idx = max_bin;
        if (max_bin > remaining_segments)
            max_idx = max_bin - remaining_segments;
        if (boundary_index < min_idx)
            boundary_index = min_idx;
        if (boundary_index > max_idx)
            boundary_index = max_idx;
        boundaries[i] = boundary_index;
    }
    return boundaries;
}

void MusicPlayerLibrary::FFTExecuter::MapFreqToSegments(
    const std::vector<float>& fft_result,
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
        const size_t ring_buffer_max_size = GetRingBufferMaxSize();
        // 检查缓冲区是否有足够数据
        if (spectrum_data_ring_buffer.size() < ring_buffer_max_size || !ring_buffer_has_unprocessed_data)
            return;

        // drain data
        raw_samples.assign(
            spectrum_data_ring_buffer.begin(),
            spectrum_data_ring_buffer.begin() + ring_buffer_max_size);
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

    {
        const auto boundaries = GenBoundaries(static_cast<float>(sample_rate), fft_size, segment_num);
        std::lock_guard lock(spectrum_data_mutex); 
        spectrum_data.clear();
        MapFreqToSegments(fft_result, spectrum_data, boundaries);

        for (size_t i = 0; i < spectrum_data.size(); ++i) {
            float& val = spectrum_data[i];
            val *= static_cast<float>(BASE_FFT_SIZE) / static_cast<float>(fft_size);
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

        // Push the computed frame into the timestamped delay queue.
        spectrum_delay_queue.push_back({ spectrum_data, std::chrono::steady_clock::now() });
        while (spectrum_delay_queue.size() > MAX_DELAY_QUEUE_SIZE)
            spectrum_delay_queue.pop_front();
    }
   
}


const std::vector<float> MusicPlayerLibrary::FFTExecuter::GetAudioFFTData()
{
    std::lock_guard lock(spectrum_data_mutex);
    if (spectrum_delay_queue.empty())
        return {};

    const auto target_time = std::chrono::steady_clock::now()
        - std::chrono::milliseconds(delay_ms.load());

    size_t target = spectrum_delay_queue.size() - 1;
    for (size_t i = 0; i < spectrum_delay_queue.size(); ++i)
    {
        if (spectrum_delay_queue[i].captured_at > target_time)
        {
            target = i == 0 ? 0 : i - 1;
            break;
        }
    }
    return spectrum_delay_queue[target].data;
}

void MusicPlayerLibrary::FFTExecuter::ResetBuffers()
{
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
        const size_t ring_buffer_max_size = GetRingBufferMaxSize();
        // FFT execution limitd to 60fps
        ring_buffer_cv.wait(lock, [this]()
            {
                return !fft_thread_running ||
                    (ring_buffer_has_unprocessed_data &&
                        spectrum_data_ring_buffer.size() >= GetRingBufferMaxSize());
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
                spectrum_data_ring_buffer.size() < ring_buffer_max_size)
                continue;
        }

        lock.unlock();
        ExecuteAudioFFT();
        next_fft_time = std::chrono::steady_clock::now() + std::chrono::milliseconds(FFT_FRAME_INTERVAL_MS);
    }
}

MusicPlayerLibrary::FFTExecuter::FFTExecuter(int in_sample_rate):
    fft_size(SelectFftSize(in_sample_rate)),
    sample_rate(in_sample_rate)
{
    NATIVE_TRACE("info: fft executer initialized!");
    NATIVE_TRACE("info: sample rate = %d, fft_size = %d, ring buffer max size = %d",
        sample_rate,
        static_cast<int>(fft_size),
        static_cast<int>(GetRingBufferMaxSize()));
    fft_cfg = kiss_fft_alloc(static_cast<int>(fft_size), 0, nullptr, nullptr);
    if (!fft_cfg)
        throw std::runtime_error("kiss_fft_alloc failed!");

    fft_in.resize(fft_size);
    fft_out.resize(fft_size);
    StartFFTThread();
}

MusicPlayerLibrary::FFTExecuter::~FFTExecuter()
{
    StopFFTThread();
    if (fft_cfg)
    {
        kiss_fft_free(fft_cfg);
        fft_cfg = nullptr;
    }
}
