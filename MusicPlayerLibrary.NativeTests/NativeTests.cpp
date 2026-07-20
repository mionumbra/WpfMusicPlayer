#define DOCTEST_CONFIG_USE_STD_HEADERS
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#define NATIVE_REQUIRE(condition, ...) \
    DOCTEST_REQUIRE_MESSAGE(static_cast<bool>(condition), __VA_ARGS__)

#include "Audio/AudioOutputFormat.h"
#include "Audio/DSP/EqualizerDsp.h"
#include "Audio/DSP/FapoEqualizer.h"
#include "Audio/FFT/AudioPipelinePerformanceHelper.h"
#include "Audio/FFT/FFTExecuter.h"

#include <FAPOBase.h>

extern "C"
{
#include <libavfilter/avfilter.h>
#include <libavutil/audio_fifo.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <limits>
#include <memory>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace
{
    using namespace MusicPlayerLibrary::AudioDsp;
    using MusicPlayerLibrary::GetAudioPipelineBufferingProfile;
    using MusicPlayerLibrary::SelectAudioPipelineBufferingProfile;

    constexpr FAudioGUID PcmSubFormat{
        0x00000001, 0x0000, 0x0010,
        {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}
    };
    constexpr FAudioGUID IeeeFloatSubFormat{
        0x00000003, 0x0000, 0x0010,
        {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}
    };

    bool SameGuid(const FAudioGUID& left, const FAudioGUID& right) noexcept
    {
        return std::memcmp(&left, &right, sizeof(FAudioGUID)) == 0;
    }

    void RequireClose(float expected, float actual, float tolerance,
                      std::string_view message)
    {
        NATIVE_REQUIRE(
            std::isfinite(actual) && std::abs(expected - actual) <= tolerance,
            message);
    }

    struct FapoReleaser
    {
        void operator()(FAPO* effect) const noexcept
        {
            if (effect != nullptr)
                effect->Release(effect);
        }
    };
    using UniqueFapo = std::unique_ptr<FAPO, FapoReleaser>;

    struct FilterGraphReleaser
    {
        void operator()(AVFilterGraph* graph) const noexcept
        {
            if (graph != nullptr)
                avfilter_graph_free(&graph);
        }
    };
    using UniqueFilterGraph = std::unique_ptr<AVFilterGraph, FilterGraphReleaser>;

    struct SwrContextReleaser
    {
        void operator()(SwrContext* context) const noexcept
        {
            if (context != nullptr)
                swr_free(&context);
        }
    };
    using UniqueSwrContext = std::unique_ptr<SwrContext, SwrContextReleaser>;

    struct AudioFifoReleaser
    {
        void operator()(AVAudioFifo* fifo) const noexcept
        {
            if (fifo != nullptr)
                av_audio_fifo_free(fifo);
        }
    };
    using UniqueAudioFifo = std::unique_ptr<AVAudioFifo, AudioFifoReleaser>;

    UniqueFapo MakeFapo(
        const EqualizerDspSnapshot& initial,
        const LimiterConfig& limiter)
    {
        FAPO* effect = nullptr;
        NATIVE_REQUIRE(CreateEqualizerFapo(initial, limiter, &effect) == FAUDIO_OK,
                "CreateEqualizerFapo failed");
        NATIVE_REQUIRE(effect != nullptr, "CreateEqualizerFapo returned a null effect");
        return UniqueFapo(effect);
    }

    FAudioWaveFormatExtensible MakeFloatFormat(
        std::uint32_t sampleRate = 48'000,
        std::uint16_t channelCount = 2)
    {
        FAudioWaveFormatExtensible format{};
        format.Format.wFormatTag = FAUDIO_FORMAT_EXTENSIBLE;
        format.Format.nChannels = channelCount;
        format.Format.nSamplesPerSec = sampleRate;
        format.Format.nBlockAlign = static_cast<std::uint16_t>(
            channelCount * sizeof(float));
        format.Format.nAvgBytesPerSec =
            sampleRate * format.Format.nBlockAlign;
        format.Format.wBitsPerSample = 32;
        format.Format.cbSize = static_cast<std::uint16_t>(
            sizeof(FAudioWaveFormatExtensible) - sizeof(FAudioWaveFormatEx));
        format.Samples.wValidBitsPerSample = 32;
        format.dwChannelMask = channelCount == 2
            ? SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT
            : 0;
        format.SubFormat = IeeeFloatSubFormat;
        return format;
    }

    FAudioWaveFormatExtensible MakePcm24Format(
        std::uint32_t sampleRate = 48'000,
        std::uint16_t channelCount = 2,
        std::uint16_t containerBits = 32)
    {
        FAudioWaveFormatExtensible format{};
        format.Format.wFormatTag = FAUDIO_FORMAT_EXTENSIBLE;
        format.Format.nChannels = channelCount;
        format.Format.nSamplesPerSec = sampleRate;
        format.Format.nBlockAlign = static_cast<std::uint16_t>(
            channelCount * containerBits / 8);
        format.Format.nAvgBytesPerSec =
            sampleRate * format.Format.nBlockAlign;
        format.Format.wBitsPerSample = containerBits;
        format.Format.cbSize = static_cast<std::uint16_t>(
            sizeof(FAudioWaveFormatExtensible) - sizeof(FAudioWaveFormatEx));
        format.Samples.wValidBitsPerSample = 24;
        format.dwChannelMask = channelCount == 2 ? SPEAKER_STEREO : 0;
        format.SubFormat = PcmSubFormat;
        return format;
    }

    void RequirePackedFrameLayoutMatchesFaudio(
        const MusicPlayerLibrary::AudioOutputFormat& format,
        int frameCount)
    {
        const int bytesPerSample = av_get_bytes_per_sample(format.sample_format);
        NATIVE_REQUIRE(bytesPerSample > 0 && !av_sample_fmt_is_planar(format.sample_format),
                "resolved output must use a valid packed FFmpeg sample format");
        NATIVE_REQUIRE(format.wave_format.Format.nBlockAlign ==
                    format.channel_count * bytesPerSample,
                "FFmpeg and FAudio disagree on the output frame size");
        NATIVE_REQUIRE(format.wave_format.Format.nAvgBytesPerSec ==
                    format.sample_rate * format.wave_format.Format.nBlockAlign,
                "FAudio average byte rate does not match its frame size");

        int lineSize = 0;
        const int fifoBufferBytes = av_samples_get_buffer_size(
            &lineSize, format.channel_count, frameCount,
            format.sample_format, 1);
        const int faudioBufferBytes =
            frameCount * format.wave_format.Format.nBlockAlign;
        NATIVE_REQUIRE(fifoBufferBytes == faudioBufferBytes &&
                    lineSize == faudioBufferBytes,
                "FFmpeg FIFO buffer bytes do not match FAudio frame bytes");
    }

    MusicPlayerLibrary::AudioOutputFormat MakeResolvedOutputFormat(
        MusicPlayerLibrary::AudioChannelMode channelMode,
        MusicPlayerLibrary::AudioBitDepth bitDepth,
        int sampleRate = 48'000)
    {
        MusicPlayerLibrary::AudioOutputFormat requested{};
        requested.requested_sample_rate = sampleRate;
        requested.requested_channel_mode = channelMode;
        requested.requested_bit_depth = bitDepth;
        return MusicPlayerLibrary::ResolveAudioOutputFormat(
            requested, MakeFloatFormat(sampleRate));
    }

    std::vector<std::uint8_t> ReferenceSwrFftConversion(
        const MusicPlayerLibrary::AudioOutputFormat& format,
        const std::uint8_t* input,
        int frameCount)
    {
        AVChannelLayout inputLayout{};
        if (format.channel_mask != 0)
            NATIVE_REQUIRE(av_channel_layout_from_mask(&inputLayout, format.channel_mask) >= 0,
                    "reference swresample input layout failed");
        else
            av_channel_layout_default(&inputLayout, format.channel_count);
        NATIVE_REQUIRE(inputLayout.nb_channels == format.channel_count,
                "reference swresample channel count mismatch");

        AVChannelLayout fftLayout = AV_CHANNEL_LAYOUT_STEREO;
        SwrContext* rawContext = nullptr;
        const int allocationResult = swr_alloc_set_opts2(
            &rawContext,
            &fftLayout,
            AV_SAMPLE_FMT_S16,
            48'000,
            &inputLayout,
            format.sample_format,
            format.sample_rate,
            0,
            nullptr);
        av_channel_layout_uninit(&fftLayout);
        av_channel_layout_uninit(&inputLayout);
        NATIVE_REQUIRE(allocationResult >= 0 && rawContext != nullptr,
                "reference swr_alloc_set_opts2 failed");
        UniqueSwrContext context(rawContext);
        NATIVE_REQUIRE(swr_init(context.get()) >= 0, "reference swr_init failed");

        const int outputCapacity = swr_get_out_samples(context.get(), frameCount);
        NATIVE_REQUIRE(outputCapacity >= 0, "reference swr_get_out_samples failed");
        std::vector<std::uint8_t> output(
            static_cast<std::size_t>(outputCapacity) * 2 * sizeof(std::int16_t));
        const std::uint8_t* inputPlanes[] = {input};
        std::uint8_t* outputPlanes[] = {output.data()};
        const int converted = swr_convert(
            context.get(), outputPlanes, outputCapacity,
            inputPlanes, frameCount);
        NATIVE_REQUIRE(converted >= 0, "reference swr_convert failed");
        output.resize(static_cast<std::size_t>(converted) * 2 * sizeof(std::int16_t));
        return output;
    }

    class FFTExecuterProbe final : public MusicPlayerLibrary::FFTExecuter
    {
    public:
        explicit FFTExecuterProbe(const MusicPlayerLibrary::AudioOutputFormat& format)
            : FFTExecuter(format)
        {
            StopFFTThread();
            fft_in.resize(fft_size);
            fft_out.resize(fft_size);
        }

        [[nodiscard]] std::size_t FrameCount() const noexcept
        {
            return fft_size;
        }

        [[nodiscard]] std::size_t FrameByteCount() const noexcept
        {
            return fft_size * 2 * sizeof(std::int16_t);
        }

        [[nodiscard]] int SampleRate() const noexcept
        {
            return sample_rate;
        }

        [[nodiscard]] std::vector<std::size_t> Boundaries()
        {
            return GenBoundaries(
                static_cast<float>(sample_rate), fft_size,
                static_cast<std::size_t>(32), 20.0f, 20'000.0f);
        }

        [[nodiscard]] std::vector<double> Window(
            const std::vector<std::uint8_t>& samples)
        {
            std::vector<double> windowed;
            ApplyWindow(samples, windowed);
            return windowed;
        }

        [[nodiscard]] std::vector<float> Analyze(
            const std::vector<std::uint8_t>& samples)
        {
            NATIVE_REQUIRE(samples.size() == FrameByteCount(),
                    "FFT probe input must contain exactly one analysis frame");
            {
                std::lock_guard lock(ring_buffer_mutex);
                spectrum_data_ring_buffer.assign(samples.begin(), samples.end());
                ring_buffer_has_unprocessed_data = true;
            }
            ExecuteAudioFFT();

            std::vector<float> result(32);
            const int copied = CopyAudioFFTData(
                result.data(), static_cast<int>(result.size()));
            NATIVE_REQUIRE(copied == static_cast<int>(result.size()),
                    "FFT probe did not return all legacy spectrum bands");
            return result;
        }
    };

    std::uint32_t LockFapo(
        FAPO* effect,
        const FAudioWaveFormatExtensible& inputFormat,
        const FAudioWaveFormatExtensible& outputFormat,
        std::uint32_t maxFrameCount = 1'024,
        std::uint32_t inputCount = 1,
        std::uint32_t outputCount = 1,
        std::uint32_t outputMaxFrameCount = 0)
    {
        const FAPOLockForProcessBufferParameters input{
            &inputFormat.Format, maxFrameCount};
        const FAPOLockForProcessBufferParameters output{
            &outputFormat.Format,
            outputMaxFrameCount == 0 ? maxFrameCount : outputMaxFrameCount};
        return effect->LockForProcess(
            effect, inputCount, &input, outputCount, &output);
    }

    FAPOProcessBufferParameters ProcessFapo(
        FAPO* effect,
        float* input,
        FAPOBufferFlags inputFlags,
        float* output,
        std::uint32_t frameCount,
        bool enabled)
    {
        const FAPOProcessBufferParameters inputParameters{
            input, inputFlags, frameCount};
        FAPOProcessBufferParameters outputParameters{
            output, FAPO_BUFFER_SILENT, 0};
        effect->Process(
            effect, 1, &inputParameters, 1, &outputParameters, enabled ? 1 : 0);
        return outputParameters;
    }

    std::vector<float> StereoSine(
        std::uint32_t rate, float frequency, float amplitude, std::uint32_t frames)
    {
        std::vector<float> result(frames * 2);
        for (std::uint32_t frame = 0; frame < frames; ++frame)
        {
            const float sample = amplitude * std::sin(
                2.0f * std::numbers::pi_v<float> * frequency * frame / rate);
            result[frame * 2] = result[frame * 2 + 1] = sample;
        }
        return result;
    }

    EqualizerDsp MakePreparedDsp(std::uint32_t sampleRate, std::uint32_t frameCount)
    {
        EqualizerDsp dsp;
        LimiterConfig limiter;
        limiter.enabled = false;
        NATIVE_REQUIRE(dsp.Prepare(sampleRate, 2, frameCount, limiter),
                "EqualizerDsp::Prepare rejected a supported stereo format");
        return dsp;
    }

    EqualizerDsp MakePreparedLimiter(
        std::uint32_t sampleRate,
        std::uint32_t frameCount,
        std::uint32_t channelCount = 2)
    {
        EqualizerDsp dsp;
        LimiterConfig limiter;
        NATIVE_REQUIRE(dsp.Prepare(sampleRate, channelCount, frameCount, limiter),
                "EqualizerDsp::Prepare rejected a supported limiter format");
        return dsp;
    }

    void TestZeroDbIdentity()
    {
        constexpr std::uint32_t SampleRate = 48'000;
        constexpr std::uint32_t FrameCount = 2'048;

        auto input = StereoSine(SampleRate, 777.0f, 0.35f, FrameCount);
        for (std::uint32_t frame = 0; frame < FrameCount; ++frame)
            input[frame * 2 + 1] += 0.05f * std::cos(0.17f * frame);

        std::vector<float> output(input.size());
        auto dsp = MakePreparedDsp(SampleRate, FrameCount);
        const auto snapshot = CompileEqualizerSnapshot(
            MakeDefaultTenBandConfig(), SampleRate, 1);

        NATIVE_REQUIRE(dsp.Process(snapshot, input.data(), output.data(), FrameCount, false),
                "EqualizerDsp::Process rejected valid zero-gain input");
        for (std::size_t index = 0; index < input.size(); ++index)
            RequireClose(input[index], output[index], 1.0e-6f,
                         "zero-gain equalizer changed a sample");
    }

    void TestOneKilohertzPlusSixDb()
    {
        constexpr std::uint32_t SampleRate = 48'000;
        constexpr std::uint32_t WarmUpFrames = 4'096;
        constexpr std::uint32_t MeasuredFrames = 4'096;
        constexpr std::uint32_t FrameCount = WarmUpFrames + MeasuredFrames;

        auto config = MakeDefaultTenBandConfig();
        config.bands[5].frequency_hz = 1'000.0f;
        config.bands[5].q = 1.0f;
        config.bands[5].gain_db = 6.0f;

        const auto input = StereoSine(SampleRate, 1'000.0f, 0.1f, FrameCount);
        std::vector<float> output(input.size());
        auto dsp = MakePreparedDsp(SampleRate, FrameCount);
        const auto snapshot = CompileEqualizerSnapshot(config, SampleRate, 2);

        NATIVE_REQUIRE(dsp.Process(snapshot, input.data(), output.data(), FrameCount, false),
                "EqualizerDsp::Process rejected valid boosted input");

        double inputSquareSum = 0.0;
        double outputSquareSum = 0.0;
        for (std::uint32_t frame = WarmUpFrames; frame < FrameCount; ++frame)
        {
            const double inputSample = input[frame * 2];
            const double outputSample = output[frame * 2];
            inputSquareSum += inputSample * inputSample;
            outputSquareSum += outputSample * outputSample;
        }

        const double inputRms = std::sqrt(inputSquareSum / MeasuredFrames);
        const double outputRms = std::sqrt(outputSquareSum / MeasuredFrames);
        const float gainDb = static_cast<float>(20.0 * std::log10(outputRms / inputRms));
        RequireClose(6.0f, gainDb, 0.1f,
                     "1 kHz peaking filter did not produce the configured gain");
    }

    void TestNyquistAndConfiguredRates()
    {
        constexpr std::array<std::uint32_t, 9> SampleRates{
            8'000, 11'025, 16'000, 22'050, 44'100,
            48'000, 88'200, 96'000, 192'000
        };
        constexpr std::uint32_t FrameCount = 1'024;

        auto config = MakeDefaultTenBandConfig();
        for (auto& band : config.bands)
        {
            band.q = 1.0f;
            band.gain_db = 3.0f;
        }

        for (const auto sampleRate : SampleRates)
        {
            const auto snapshot = CompileEqualizerSnapshot(config, sampleRate, 3);
            for (const auto& coefficients : snapshot.bands)
            {
                NATIVE_REQUIRE(std::isfinite(coefficients.b0) &&
                        std::isfinite(coefficients.b1) &&
                        std::isfinite(coefficients.b2) &&
                        std::isfinite(coefficients.a1) &&
                        std::isfinite(coefficients.a2),
                        "configured-rate coefficients must remain finite");
            }

            if (sampleRate == 16'000)
            {
                const auto& coefficients = snapshot.bands[9];
                NATIVE_REQUIRE(coefficients.b0 == 1.0f && coefficients.b1 == 0.0f &&
                        coefficients.b2 == 0.0f && coefficients.a1 == 0.0f &&
                        coefficients.a2 == 0.0f,
                        "16 kHz band must be identity at a 16 kHz sample rate");
                NATIVE_REQUIRE((snapshot.enabled_mask & (1u << 9)) == 0,
                        "Nyquist-rejected 16 kHz band must not be enabled");
            }

            const auto input = StereoSine(sampleRate, 1'000.0f, 0.1f, FrameCount);
            std::vector<float> output(input.size());
            auto dsp = MakePreparedDsp(sampleRate, FrameCount);
            NATIVE_REQUIRE(dsp.Process(snapshot, input.data(), output.data(), FrameCount, false),
                    "EqualizerDsp::Process rejected a configured sample rate");
            NATIVE_REQUIRE(std::all_of(output.begin(), output.end(),
                                [](float sample) { return std::isfinite(sample); }),
                    "configured-rate output must remain finite");
        }
    }

    void TestLimiterDelayAtConfiguredRates()
    {
        struct DelayCase
        {
            std::uint32_t sample_rate;
            std::uint32_t delay_frames;
        };
        constexpr std::array<DelayCase, 2> Cases{{
            {48'000, 239},
            {44'100, 219}
        }};

        for (const auto& testCase : Cases)
        {
            const std::uint32_t frameCount = testCase.delay_frames + 2;
            std::vector<float> input(static_cast<std::size_t>(frameCount) * 2, 0.0f);
            std::vector<float> output(input.size(), -1.0f);
            input[0] = input[1] = 0.5f;

            auto dsp = MakePreparedLimiter(testCase.sample_rate, frameCount);
            const auto snapshot = CompileEqualizerSnapshot(
                MakeDefaultTenBandConfig(), testCase.sample_rate, 10);

            NATIVE_REQUIRE(dsp.GetLimiterDelayFrames() == testCase.delay_frames,
                    "limiter reported the wrong look-ahead delay");
            NATIVE_REQUIRE(dsp.Process(snapshot, input.data(), output.data(), frameCount, false),
                    "limiter rejected an impulse input");
            for (std::uint32_t frame = 0; frame < testCase.delay_frames; ++frame)
            {
                RequireClose(0.0f, output[static_cast<std::size_t>(frame) * 2],
                             1.0e-7f, "limiter impulse appeared before its delay");
                RequireClose(0.0f, output[static_cast<std::size_t>(frame) * 2 + 1],
                             1.0e-7f, "limiter impulse appeared before its delay");
            }
            RequireClose(0.5f,
                         output[static_cast<std::size_t>(testCase.delay_frames) * 2],
                         1.0e-6f, "limiter impulse appeared at the wrong frame");
            RequireClose(0.5f,
                         output[static_cast<std::size_t>(testCase.delay_frames) * 2 + 1],
                         1.0e-6f, "limiter impulse appeared at the wrong frame");
        }
    }

    void TestLimiterLinksStereoAtCeiling()
    {
        constexpr std::uint32_t SampleRate = 48'000;
        constexpr std::uint32_t DelayFrames = 239;
        constexpr std::uint32_t FrameCount = DelayFrames + 1;
        std::vector<float> input(static_cast<std::size_t>(FrameCount) * 2, 0.0f);
        std::vector<float> output(input.size(), 0.0f);
        input[0] = 1.0f;
        input[1] = 0.25f;

        auto dsp = MakePreparedLimiter(SampleRate, FrameCount);
        const auto snapshot = CompileEqualizerSnapshot(
            MakeDefaultTenBandConfig(), SampleRate, 11);
        NATIVE_REQUIRE(dsp.Process(snapshot, input.data(), output.data(), FrameCount, false),
                "limiter rejected linked-stereo input");

        RequireClose(0.70f, output[static_cast<std::size_t>(DelayFrames) * 2],
                     1.0e-6f, "limiter did not enforce its ceiling");
        RequireClose(0.175f, output[static_cast<std::size_t>(DelayFrames) * 2 + 1],
                     1.0e-6f, "limiter did not apply linked gain to stereo");
    }

    void TestLimiterDoesNotApplyMakeupGain()
    {
        constexpr std::uint32_t SampleRate = 48'000;
        constexpr std::uint32_t DelayFrames = 239;
        constexpr std::uint32_t MeasuredFrames = 32;
        constexpr std::uint32_t FrameCount = DelayFrames + MeasuredFrames;
        std::vector<float> input(static_cast<std::size_t>(FrameCount) * 2, 0.5f);
        std::vector<float> output(input.size(), 0.0f);

        auto dsp = MakePreparedLimiter(SampleRate, FrameCount);
        const auto snapshot = CompileEqualizerSnapshot(
            MakeDefaultTenBandConfig(), SampleRate, 12);
        NATIVE_REQUIRE(dsp.Process(snapshot, input.data(), output.data(), FrameCount, false),
                "limiter rejected below-ceiling input");

        for (std::uint32_t frame = DelayFrames; frame < FrameCount; ++frame)
        {
            RequireClose(0.5f, output[static_cast<std::size_t>(frame) * 2],
                         1.0e-6f, "limiter applied make-up gain");
            RequireClose(0.5f, output[static_cast<std::size_t>(frame) * 2 + 1],
                         1.0e-6f, "limiter applied make-up gain");
        }
    }

    void TestLimiterReleaseIsLinear()
    {
        constexpr std::uint32_t SampleRate = 48'000;
        constexpr std::uint32_t DelayFrames = 239;
        constexpr std::uint32_t ReleaseFrames = 2'400;
        constexpr std::uint32_t FrameCount = DelayFrames + ReleaseFrames + 1;
        std::vector<float> input(static_cast<std::size_t>(FrameCount) * 2, 0.5f);
        std::vector<float> output(input.size(), 0.0f);
        input[0] = input[1] = 1.0f;

        auto dsp = MakePreparedLimiter(SampleRate, FrameCount);
        const auto snapshot = CompileEqualizerSnapshot(
            MakeDefaultTenBandConfig(), SampleRate, 13);
        NATIVE_REQUIRE(dsp.Process(snapshot, input.data(), output.data(), FrameCount, false),
                "limiter rejected release-ramp input");

        const auto leftAt = [&output](std::uint32_t frame)
        {
            return output[static_cast<std::size_t>(frame) * 2];
        };
        const float firstReleaseGain = leftAt(DelayFrames + 1) / 0.5f;
        const float midpointGain = leftAt(DelayFrames + ReleaseFrames / 2) / 0.5f;
        const float finalGain = leftAt(DelayFrames + ReleaseFrames) / 0.5f;
        RequireClose(0.70f, leftAt(DelayFrames), 1.0e-6f,
                     "limiter peak did not begin at ceiling gain");
        RequireClose(0.70f + 0.30f / ReleaseFrames, firstReleaseGain, 2.0e-6f,
                     "limiter first release increment was not linear");
        RequireClose(0.85f, midpointGain, 2.0e-5f,
                     "limiter release midpoint was not linear");
        RequireClose(1.0f, finalGain, 2.0e-5f,
                     "limiter did not finish release in 50 ms");
    }

    void TestLimiterSilentInputDrainsTail()
    {
        constexpr std::uint32_t SampleRate = 48'000;
        constexpr std::uint32_t DelayFrames = 239;
        auto dsp = MakePreparedLimiter(SampleRate, DelayFrames);
        const auto snapshot = CompileEqualizerSnapshot(
            MakeDefaultTenBandConfig(), SampleRate, 14);
        const std::array<float, 2> impulse{0.5f, 0.5f};
        std::array<float, 2> initialOutput{};

        NATIVE_REQUIRE(dsp.Process(snapshot, impulse.data(), initialOutput.data(), 1, false),
                "limiter rejected queued tail input");
        NATIVE_REQUIRE(dsp.HasTail(), "limiter did not report a queued tail");

        std::vector<float> drained(static_cast<std::size_t>(DelayFrames) * 2, -1.0f);
        NATIVE_REQUIRE(dsp.Process(snapshot, nullptr, drained.data(), DelayFrames, true),
                "silent processing did not return the queued impulse");
        RequireClose(0.5f, drained[(static_cast<std::size_t>(DelayFrames) - 1) * 2],
                     1.0e-6f, "silent processing did not drain the queued impulse");
        RequireClose(0.5f, drained[(static_cast<std::size_t>(DelayFrames) - 1) * 2 + 1],
                     1.0e-6f, "silent processing did not drain the queued impulse");

        std::array<float, 2> finalOutput{1.0f, 1.0f};
        NATIVE_REQUIRE(!dsp.Process(snapshot, nullptr, finalOutput.data(), 1, true),
                "silent processing reported a tail after it drained");
        RequireClose(0.0f, finalOutput[0], 1.0e-7f,
                     "drained limiter emitted a nonzero sample");
        RequireClose(0.0f, finalOutput[1], 1.0e-7f,
                     "drained limiter emitted a nonzero sample");
        NATIVE_REQUIRE(!dsp.HasTail(), "limiter tail did not clear after silent draining");
    }

    void TestLimiterResetGenerationDropsTail()
    {
        constexpr std::uint32_t SampleRate = 48'000;
        constexpr std::uint32_t DelayFrames = 239;
        auto dsp = MakePreparedLimiter(SampleRate, DelayFrames + 1);
        const auto firstSnapshot = CompileEqualizerSnapshot(
            MakeDefaultTenBandConfig(), SampleRate, 15);
        const auto resetSnapshot = CompileEqualizerSnapshot(
            MakeDefaultTenBandConfig(), SampleRate, 16);
        const std::array<float, 2> impulse{0.5f, 0.5f};
        std::array<float, 2> initialOutput{};
        NATIVE_REQUIRE(dsp.Process(firstSnapshot, impulse.data(), initialOutput.data(), 1, false),
                "limiter rejected pre-reset input");
        NATIVE_REQUIRE(dsp.HasTail(), "limiter did not queue pre-reset history");

        std::vector<float> output(static_cast<std::size_t>(DelayFrames + 1) * 2, 1.0f);
        NATIVE_REQUIRE(!dsp.Process(resetSnapshot, nullptr, output.data(), DelayFrames + 1, true),
                "reset limiter reported discarded history as a tail");
        NATIVE_REQUIRE(std::all_of(output.begin(), output.end(),
                            [](float sample) { return sample == 0.0f; }),
                "reset generation did not discard queued limiter history");
        NATIVE_REQUIRE(!dsp.HasTail(), "reset generation left limiter tail state behind");
    }

    void TestLimiterIsChunkInvariant()
    {
        constexpr std::uint32_t SampleRate = 48'000;
        constexpr std::uint32_t FrameCount = 4'093;
        constexpr std::uint32_t ChunkFrames = 127;
        std::vector<float> input(static_cast<std::size_t>(FrameCount) * 2);
        for (std::uint32_t frame = 0; frame < FrameCount; ++frame)
        {
            const float carrier = 0.58f * std::sin(0.071f * frame);
            input[static_cast<std::size_t>(frame) * 2] =
                frame % 503 == 0 ? 1.2f : carrier;
            input[static_cast<std::size_t>(frame) * 2 + 1] =
                frame % 337 == 0 ? -0.95f : 0.37f * std::cos(0.043f * frame);
        }

        auto config = MakeDefaultTenBandConfig();
        config.bands[5].gain_db = 3.0f;
        const auto snapshot = CompileEqualizerSnapshot(config, SampleRate, 17);
        std::vector<float> oneBlock(input.size(), 0.0f);
        auto wholeDsp = MakePreparedLimiter(SampleRate, FrameCount);
        NATIVE_REQUIRE(wholeDsp.Process(snapshot, input.data(), oneBlock.data(), FrameCount, false),
                "limiter rejected one-block input");

        std::vector<float> chunked = input;
        auto chunkedDsp = MakePreparedLimiter(SampleRate, FrameCount);
        for (std::uint32_t offset = 0; offset < FrameCount; offset += ChunkFrames)
        {
            const std::uint32_t count = std::min(ChunkFrames, FrameCount - offset);
            float* const block = chunked.data() + static_cast<std::size_t>(offset) * 2;
            NATIVE_REQUIRE(chunkedDsp.Process(snapshot, block, block, count, false),
                    "limiter rejected a 127-frame in-place chunk");
        }

        for (std::size_t index = 0; index < oneBlock.size(); ++index)
        {
            RequireClose(oneBlock[index], chunked[index], 1.0e-6f,
                         "limiter output changed with chunk size");
        }
    }

    void TestFapoFactoryAndRegistration()
    {
        constexpr std::uint32_t SampleRate = 48'000;
        const auto snapshot = CompileEqualizerSnapshot(
            MakeDefaultTenBandConfig(), SampleRate, 20);
        LimiterConfig limiter;
        limiter.enabled = false;
        auto effect = MakeFapo(snapshot, limiter);

        NATIVE_REQUIRE(effect->AddRef(effect.get()) == 2,
                "factory reference count was not one");
        NATIVE_REQUIRE(effect->Release(effect.get()) == 1,
                "factory reference count did not return to one");

        FAPORegistrationProperties* properties = nullptr;
        NATIVE_REQUIRE(effect->GetRegistrationProperties(
                    effect.get(), &properties) == FAUDIO_OK &&
                properties != nullptr,
                "FAPO registration properties were unavailable");
        constexpr std::uint32_t RequiredFlags =
            FAPO_FLAG_CHANNELS_MUST_MATCH |
            FAPO_FLAG_FRAMERATE_MUST_MATCH |
            FAPO_FLAG_BITSPERSAMPLE_MUST_MATCH |
            FAPO_FLAG_BUFFERCOUNT_MUST_MATCH |
            FAPO_FLAG_INPLACE_SUPPORTED;
        NATIVE_REQUIRE(properties->Flags == RequiredFlags,
                "FAPO registration flags were incorrect");
        NATIVE_REQUIRE((properties->Flags & FAPO_FLAG_INPLACE_REQUIRED) == 0,
                "FAPO incorrectly required in-place processing");
        reinterpret_cast<FAPOBase*>(effect.get())->pFree(properties);
    }

    void TestFapoAdvertisesExtensibleFormats()
    {
        constexpr std::uint32_t SampleRate = 48'000;
        const auto snapshot = CompileEqualizerSnapshot(
            MakeDefaultTenBandConfig(), SampleRate, 29);
        auto effect = MakeFapo(snapshot, LimiterConfig{});
        const auto valid = MakeFloatFormat(SampleRate);

        NATIVE_REQUIRE(effect->IsInputFormatSupported(
                    effect.get(), &valid.Format, &valid.Format, nullptr) ==
                    FAUDIO_OK,
                "FAPO did not advertise its supported extensible input format");
        NATIVE_REQUIRE(effect->IsOutputFormatSupported(
                    effect.get(), &valid.Format, &valid.Format, nullptr) ==
                    FAUDIO_OK,
                "FAPO did not advertise its supported extensible output format");

        auto legacyFloat = valid;
        legacyFloat.Format.wFormatTag = FAUDIO_FORMAT_IEEE_FLOAT;
        legacyFloat.Format.cbSize = 0;
        FAudioWaveFormatEx* closestFormat = nullptr;
        NATIVE_REQUIRE(effect->IsInputFormatSupported(
                    effect.get(), &valid.Format, &legacyFloat.Format,
                    &closestFormat) == FAPO_E_FORMAT_UNSUPPORTED &&
                closestFormat == nullptr,
                "FAPO advertised an unsupported legacy input format");
        NATIVE_REQUIRE(effect->IsOutputFormatSupported(
                    effect.get(), &valid.Format, &legacyFloat.Format,
                    &closestFormat) == FAPO_E_FORMAT_UNSUPPORTED &&
                closestFormat == nullptr,
                "FAPO advertised an unsupported legacy output format");

        auto mismatched = valid;
        mismatched.dwChannelMask = 0;
        NATIVE_REQUIRE(effect->IsInputFormatSupported(
                    effect.get(), &valid.Format, &mismatched.Format, nullptr) ==
                    FAPO_E_FORMAT_UNSUPPORTED,
                "FAPO advertised a nonmatching input format pair");
        NATIVE_REQUIRE(effect->IsOutputFormatSupported(
                    effect.get(), &valid.Format, &mismatched.Format, nullptr) ==
                    FAPO_E_FORMAT_UNSUPPORTED,
                "FAPO advertised a nonmatching output format pair");

        NATIVE_REQUIRE(effect->IsInputFormatSupported(
                    effect.get(), nullptr, &valid.Format, nullptr) ==
                    FAUDIO_E_INVALID_ARG,
                "input-format probing did not safely reject a null output format");
        NATIVE_REQUIRE(effect->IsOutputFormatSupported(
                    effect.get(), nullptr, &valid.Format, nullptr) ==
                    FAUDIO_E_INVALID_ARG,
                "output-format probing did not safely reject a null input format");
    }

    void TestFapoInPlaceMatchesOutOfPlace()
    {
        constexpr std::uint32_t SampleRate = 48'000;
        constexpr std::uint32_t FrameCount = 2'048;
        const auto initial = CompileEqualizerSnapshot(
            MakeDefaultTenBandConfig(), SampleRate, 21);
        auto config = MakeDefaultTenBandConfig();
        config.bands[5].gain_db = 6.0f;
        const auto updated = CompileEqualizerSnapshot(config, SampleRate, 22);
        LimiterConfig limiter;
        limiter.enabled = false;
        auto inPlaceEffect = MakeFapo(initial, limiter);
        auto outOfPlaceEffect = MakeFapo(initial, limiter);
        inPlaceEffect->SetParameters(
            inPlaceEffect.get(), &updated, sizeof(updated));
        outOfPlaceEffect->SetParameters(
            outOfPlaceEffect.get(), &updated, sizeof(updated));

        const auto format = MakeFloatFormat(SampleRate);
        NATIVE_REQUIRE(LockFapo(inPlaceEffect.get(), format, format, FrameCount) == FAUDIO_OK,
                "in-place FAPO lock failed");
        NATIVE_REQUIRE(LockFapo(outOfPlaceEffect.get(), format, format, FrameCount) == FAUDIO_OK,
                "out-of-place FAPO lock failed");

        auto input = StereoSine(SampleRate, 1'000.0f, 0.1f, FrameCount);
        for (std::uint32_t frame = 0; frame < FrameCount; ++frame)
            input[static_cast<std::size_t>(frame) * 2 + 1] +=
                0.03f * std::cos(0.11f * frame);
        auto inPlace = input;
        std::vector<float> outOfPlace(
            input.size(), std::numeric_limits<float>::quiet_NaN());

        const auto inPlaceResult = ProcessFapo(
            inPlaceEffect.get(), inPlace.data(), FAPO_BUFFER_VALID,
            inPlace.data(), FrameCount, true);
        const auto outOfPlaceResult = ProcessFapo(
            outOfPlaceEffect.get(), input.data(), FAPO_BUFFER_VALID,
            outOfPlace.data(), FrameCount, true);
        NATIVE_REQUIRE(inPlaceResult.BufferFlags == FAPO_BUFFER_VALID &&
                outOfPlaceResult.BufferFlags == FAPO_BUFFER_VALID &&
                inPlaceResult.ValidFrameCount == FrameCount &&
                outOfPlaceResult.ValidFrameCount == FrameCount,
                "FAPO did not report the complete valid output");
        for (std::size_t index = 0; index < inPlace.size(); ++index)
        {
            RequireClose(inPlace[index], outOfPlace[index], 1.0e-6f,
                         "in-place and out-of-place FAPO output differed");
        }
        NATIVE_REQUIRE(std::any_of(
                    outOfPlace.begin(), outOfPlace.end(),
                    [&input, index = std::size_t{0}](float sample) mutable
                    { return sample != input[index++]; }),
                "full-size parameter update was not applied");

        inPlaceEffect->UnlockForProcess(inPlaceEffect.get());
        outOfPlaceEffect->UnlockForProcess(outOfPlaceEffect.get());
    }

    void TestFapoDisabledValidPassThrough()
    {
        constexpr std::uint32_t SampleRate = 48'000;
        constexpr std::uint32_t FrameCount = 257;
        auto config = MakeDefaultTenBandConfig();
        config.bands[5].gain_db = 9.0f;
        const auto snapshot = CompileEqualizerSnapshot(config, SampleRate, 23);
        auto effect = MakeFapo(snapshot, LimiterConfig{});
        const auto format = MakeFloatFormat(SampleRate);
        NATIVE_REQUIRE(LockFapo(effect.get(), format, format, FrameCount) == FAUDIO_OK,
                "disabled pass-through FAPO lock failed");

        auto input = StereoSine(SampleRate, 777.0f, 0.37f, FrameCount);
        std::vector<float> output(
            input.size(), std::numeric_limits<float>::quiet_NaN());
        const auto result = ProcessFapo(
            effect.get(), input.data(), FAPO_BUFFER_VALID,
            output.data(), FrameCount, false);
        NATIVE_REQUIRE(result.BufferFlags == FAPO_BUFFER_VALID &&
                result.ValidFrameCount == FrameCount,
                "disabled pass-through did not preserve valid metadata");
        NATIVE_REQUIRE(output == input,
                "disabled out-of-place FAPO was not an exact pass-through");
        effect->UnlockForProcess(effect.get());
    }

    void TestFapoRejectsUnsafeProcessBuffers()
    {
        constexpr std::uint32_t SampleRate = 48'000;
        constexpr std::uint32_t LockedFrameCount = 1;
        const auto snapshot = CompileEqualizerSnapshot(
            MakeDefaultTenBandConfig(), SampleRate, 30);
        LimiterConfig limiter;
        limiter.enabled = false;
        auto effect = MakeFapo(snapshot, limiter);
        const auto format = MakeFloatFormat(SampleRate);
        NATIVE_REQUIRE(LockFapo(
                    effect.get(), format, format, LockedFrameCount) == FAUDIO_OK,
                "process-bounds FAPO lock failed");

        std::array<float, 4> oversizedInput{0.1f, 0.2f, 0.3f, 0.4f};
        std::array<float, 4> guardedOutput{-1.0f, -2.0f, 1234.0f, 5678.0f};
        const auto originalGuardedOutput = guardedOutput;
        const auto oversizedResult = ProcessFapo(
            effect.get(), oversizedInput.data(), FAPO_BUFFER_VALID,
            guardedOutput.data(), LockedFrameCount + 1, false);
        NATIVE_REQUIRE(oversizedResult.BufferFlags == FAPO_BUFFER_SILENT &&
                oversizedResult.ValidFrameCount == 0,
                "FAPO did not reject a frame count larger than its lock");
        NATIVE_REQUIRE(guardedOutput == originalGuardedOutput,
                "rejected oversized processing touched output or its sentinel");

        std::array<float, 2> output{11.0f, 12.0f};
        const auto originalOutput = output;
        const auto nullInputResult = ProcessFapo(
            effect.get(), nullptr, FAPO_BUFFER_VALID,
            output.data(), LockedFrameCount, false);
        NATIVE_REQUIRE(nullInputResult.BufferFlags == FAPO_BUFFER_SILENT &&
                nullInputResult.ValidFrameCount == 0 && output == originalOutput,
                "FAPO did not safely reject a null VALID input buffer");

        const auto nullOutputResult = ProcessFapo(
            effect.get(), oversizedInput.data(), FAPO_BUFFER_VALID,
            nullptr, LockedFrameCount, false);
        NATIVE_REQUIRE(nullOutputResult.BufferFlags == FAPO_BUFFER_SILENT &&
                nullOutputResult.ValidFrameCount == 0,
                "FAPO did not safely reject a required null output buffer");

        const auto silentResult = ProcessFapo(
            effect.get(), nullptr, FAPO_BUFFER_SILENT,
            output.data(), LockedFrameCount, false);
        NATIVE_REQUIRE(silentResult.BufferFlags == FAPO_BUFFER_SILENT &&
                silentResult.ValidFrameCount == LockedFrameCount &&
                output[0] == 0.0f && output[1] == 0.0f,
                "FAPO incorrectly required input storage for SILENT input");
        effect->UnlockForProcess(effect.get());
    }

    void TestFapoSilentInputNeverReadsBuffer()
    {
        constexpr std::uint32_t SampleRate = 48'000;
        constexpr std::uint32_t FrameCount = 127;
        auto config = MakeDefaultTenBandConfig();
        config.bands[5].gain_db = 6.0f;
        const auto snapshot = CompileEqualizerSnapshot(config, SampleRate, 24);
        LimiterConfig limiter;
        limiter.enabled = false;
        const auto format = MakeFloatFormat(SampleRate);
        std::vector<float> nanInput(
            static_cast<std::size_t>(FrameCount) * 2,
            std::numeric_limits<float>::quiet_NaN());

        for (const bool enabled : {false, true})
        {
            auto effect = MakeFapo(snapshot, limiter);
            NATIVE_REQUIRE(LockFapo(effect.get(), format, format, FrameCount) == FAUDIO_OK,
                    "silent-input FAPO lock failed");
            std::vector<float> output(
                nanInput.size(), std::numeric_limits<float>::quiet_NaN());
            const auto result = ProcessFapo(
                effect.get(), nanInput.data(), FAPO_BUFFER_SILENT,
                output.data(), FrameCount, enabled);
            NATIVE_REQUIRE(result.BufferFlags == FAPO_BUFFER_SILENT &&
                    result.ValidFrameCount == FrameCount,
                    "drained silent input was not reported silent");
            NATIVE_REQUIRE(std::all_of(
                        output.begin(), output.end(),
                        [](float sample) { return sample == 0.0f; }),
                    "silent NaN input was read or output was not fully written");
            effect->UnlockForProcess(effect.get());
        }
    }

    void TestFapoRejectsInvalidParameterBlocks()
    {
        constexpr std::uint32_t SampleRate = 48'000;
        constexpr std::uint32_t FrameCount = 1'024;
        const auto initial = CompileEqualizerSnapshot(
            MakeDefaultTenBandConfig(), SampleRate, 25);
        auto config = MakeDefaultTenBandConfig();
        config.bands[5].gain_db = 12.0f;
        const auto validUpdate = CompileEqualizerSnapshot(config, SampleRate, 26);
        LimiterConfig limiter;
        limiter.enabled = false;
        const auto format = MakeFloatFormat(SampleRate);
        const auto input = StereoSine(SampleRate, 1'000.0f, 0.1f, FrameCount);

        const auto verifyRejected = [&](EqualizerDspSnapshot update,
                                        std::uint32_t byteCount,
                                        std::string_view message)
        {
            auto effect = MakeFapo(initial, limiter);
            effect->SetParameters(effect.get(), &update, byteCount);
            NATIVE_REQUIRE(LockFapo(effect.get(), format, format, FrameCount) == FAUDIO_OK,
                    "parameter-validation FAPO lock failed");
            std::vector<float> output(
                input.size(), std::numeric_limits<float>::quiet_NaN());
            const auto result = ProcessFapo(
                effect.get(), const_cast<float*>(input.data()), FAPO_BUFFER_VALID,
                output.data(), FrameCount, true);
            NATIVE_REQUIRE(result.BufferFlags == FAPO_BUFFER_VALID && output == input,
                    message);
            effect->UnlockForProcess(effect.get());
        };

        verifyRejected(
            validUpdate, sizeof(EqualizerDspSnapshot) - 1,
            "short FAPO parameter block changed parameters");
        auto wrongByteSize = validUpdate;
        wrongByteSize.byte_size = sizeof(EqualizerDspSnapshot) - 1;
        verifyRejected(
            wrongByteSize, sizeof(wrongByteSize),
            "FAPO accepted a snapshot with the wrong byte_size");
        auto wrongAbi = validUpdate;
        ++wrongAbi.abi_version;
        verifyRejected(
            wrongAbi, sizeof(wrongAbi),
            "FAPO accepted a snapshot with the wrong ABI version");
    }

    void TestFapoGetParametersValidatesDestination()
    {
        constexpr std::uint32_t SampleRate = 48'000;
        constexpr std::uint8_t Sentinel = 0xa5;
        const auto initial = CompileEqualizerSnapshot(
            MakeDefaultTenBandConfig(), SampleRate, 31);
        auto effect = MakeFapo(initial, LimiterConfig{});

        std::array<
            std::uint8_t,
            sizeof(EqualizerDspSnapshot) + sizeof(std::uint32_t)> oversized{};
        oversized.fill(Sentinel);
        effect->GetParameters(
            effect.get(), oversized.data(),
            static_cast<std::uint32_t>(oversized.size()));
        NATIVE_REQUIRE(std::all_of(
                    oversized.begin(), oversized.end(),
                    [](std::uint8_t value) { return value == Sentinel; }),
                "FAPO GetParameters accepted an oversized destination");

        std::array<std::uint8_t, sizeof(EqualizerDspSnapshot)> undersized{};
        undersized.fill(Sentinel);
        effect->GetParameters(
            effect.get(), undersized.data(),
            sizeof(EqualizerDspSnapshot) - 1);
        NATIVE_REQUIRE(std::all_of(
                    undersized.begin(), undersized.end(),
                    [](std::uint8_t value) { return value == Sentinel; }),
                "FAPO GetParameters accepted an undersized destination");

        EqualizerDspSnapshot actual{};
        effect->GetParameters(effect.get(), &actual, sizeof(actual));
        NATIVE_REQUIRE(std::memcmp(&actual, &initial, sizeof(actual)) == 0,
                "FAPO GetParameters did not return the exact current snapshot");

        effect->GetParameters(
            effect.get(), nullptr, sizeof(EqualizerDspSnapshot));
    }

    void TestFapoLockRejectsInvalidFormats()
    {
        constexpr std::uint32_t SampleRate = 48'000;
        const auto snapshot = CompileEqualizerSnapshot(
            MakeDefaultTenBandConfig(), SampleRate, 27);
        auto effect = MakeFapo(snapshot, LimiterConfig{});
        const auto valid = MakeFloatFormat(SampleRate);

        NATIVE_REQUIRE(LockFapo(effect.get(), valid, valid, 128, 0, 1) != FAUDIO_OK,
                "FAPO accepted zero input buffers");
        NATIVE_REQUIRE(LockFapo(effect.get(), valid, valid, 128, 1, 2) != FAUDIO_OK,
                "FAPO accepted two output buffers");

        auto mismatchedChannels = valid;
        mismatchedChannels.Format.nChannels = 1;
        mismatchedChannels.Format.nBlockAlign = sizeof(float);
        mismatchedChannels.Format.nAvgBytesPerSec = SampleRate * sizeof(float);
        mismatchedChannels.dwChannelMask = 0;
        NATIVE_REQUIRE(LockFapo(effect.get(), valid, mismatchedChannels) != FAUDIO_OK,
                "FAPO accepted mismatched channel formats");

        auto legacyFloat = valid;
        legacyFloat.Format.wFormatTag = FAUDIO_FORMAT_IEEE_FLOAT;
        legacyFloat.Format.cbSize = 0;
        NATIVE_REQUIRE(LockFapo(effect.get(), legacyFloat, legacyFloat) != FAUDIO_OK,
                "FAPO accepted a non-extensible float format");

        auto pcm = valid;
        pcm.SubFormat = PcmSubFormat;
        NATIVE_REQUIRE(LockFapo(effect.get(), pcm, pcm) != FAUDIO_OK,
                "FAPO accepted a non-float32 extensible format");

        auto mismatchedRate = valid;
        mismatchedRate.Format.nSamplesPerSec = 44'100;
        mismatchedRate.Format.nAvgBytesPerSec =
            mismatchedRate.Format.nBlockAlign * 44'100;
        NATIVE_REQUIRE(LockFapo(effect.get(), valid, mismatchedRate) != FAUDIO_OK,
                "FAPO accepted mismatched sample rates");

        auto mismatchedMask = valid;
        mismatchedMask.dwChannelMask = 0;
        NATIVE_REQUIRE(LockFapo(effect.get(), valid, mismatchedMask) != FAUDIO_OK,
                "FAPO accepted nonmatching extensible formats");
        NATIVE_REQUIRE(LockFapo(effect.get(), valid, valid, 128, 1, 1, 256) != FAUDIO_OK,
                "FAPO accepted mismatched maximum frame counts");

        NATIVE_REQUIRE(LockFapo(effect.get(), valid, valid) == FAUDIO_OK,
                "FAPO rejected an exact float32 extensible format pair");
        effect->UnlockForProcess(effect.get());
    }

    void TestFapoSilentInputDrainsTail()
    {
        constexpr std::uint32_t SampleRate = 48'000;
        constexpr std::uint32_t DelayFrames = 239;
        const auto snapshot = CompileEqualizerSnapshot(
            MakeDefaultTenBandConfig(), SampleRate, 28);
        auto effect = MakeFapo(snapshot, LimiterConfig{});
        const auto format = MakeFloatFormat(SampleRate);
        NATIVE_REQUIRE(LockFapo(effect.get(), format, format, DelayFrames) == FAUDIO_OK,
                "tail-drain FAPO lock failed");

        std::array<float, 2> impulse{0.5f, 0.5f};
        std::array<float, 2> initialOutput{
            std::numeric_limits<float>::quiet_NaN(),
            std::numeric_limits<float>::quiet_NaN()};
        const auto initialResult = ProcessFapo(
            effect.get(), impulse.data(), FAPO_BUFFER_VALID,
            initialOutput.data(), 1, true);
        NATIVE_REQUIRE(initialResult.BufferFlags == FAPO_BUFFER_VALID,
                "valid input did not keep FAPO output valid");

        std::vector<float> silentInput(
            static_cast<std::size_t>(DelayFrames) * 2,
            std::numeric_limits<float>::quiet_NaN());
        std::vector<float> drained(
            silentInput.size(), std::numeric_limits<float>::quiet_NaN());
        const auto drainResult = ProcessFapo(
            effect.get(), silentInput.data(), FAPO_BUFFER_SILENT,
            drained.data(), DelayFrames, true);
        NATIVE_REQUIRE(drainResult.BufferFlags == FAPO_BUFFER_VALID,
                "FAPO stopped reporting valid output before its tail ended");
        RequireClose(0.5f, drained[(static_cast<std::size_t>(DelayFrames) - 1) * 2],
                     1.0e-6f, "FAPO did not drain its limiter tail");

        std::array<float, 2> finalInput{
            std::numeric_limits<float>::quiet_NaN(),
            std::numeric_limits<float>::quiet_NaN()};
        std::array<float, 2> finalOutput{
            std::numeric_limits<float>::quiet_NaN(),
            std::numeric_limits<float>::quiet_NaN()};
        const auto finalResult = ProcessFapo(
            effect.get(), finalInput.data(), FAPO_BUFFER_SILENT,
            finalOutput.data(), 1, true);
        NATIVE_REQUIRE(finalResult.BufferFlags == FAPO_BUFFER_SILENT &&
                finalOutput[0] == 0.0f && finalOutput[1] == 0.0f,
                "FAPO did not become silent after its tail ended");
        effect->UnlockForProcess(effect.get());
    }

    void TestAudioPipelineBufferingProfiles()
    {
        const auto fast = SelectAudioPipelineBufferingProfile(15.0);
        NATIVE_REQUIRE(fast.fifo_target_milliseconds == 80 &&
            fast.decoded_queue_target_milliseconds == 24,
            "fast CPU buffering profile changed unexpectedly");

        const auto balanced = SelectAudioPipelineBufferingProfile(15.01);
        NATIVE_REQUIRE(balanced.fifo_target_milliseconds == 140 &&
            balanced.decoded_queue_target_milliseconds == 32,
            "balanced CPU buffering profile changed unexpectedly");

        const auto slow = SelectAudioPipelineBufferingProfile(50.01);
        NATIVE_REQUIRE(slow.fifo_target_milliseconds == 220 &&
            slow.decoded_queue_target_milliseconds == 48,
            "slow CPU buffering profile changed unexpectedly");

        const auto fallback = SelectAudioPipelineBufferingProfile(
            std::numeric_limits<double>::quiet_NaN());
        NATIVE_REQUIRE(fallback.fifo_target_milliseconds == 140 &&
            fallback.decoded_queue_target_milliseconds == 32,
            "failed FFT benchmark did not select the safe fallback profile");
        NATIVE_REQUIRE(fast.fifo_target_milliseconds > fast.decoded_queue_target_milliseconds &&
            balanced.fifo_target_milliseconds > balanced.decoded_queue_target_milliseconds &&
            slow.fifo_target_milliseconds > slow.decoded_queue_target_milliseconds,
            "buffering profiles must keep most depth in the normalized PCM FIFO");

        const auto& measured = GetAudioPipelineBufferingProfile();
        NATIVE_REQUIRE(measured.fifo_target_milliseconds >
            measured.decoded_queue_target_milliseconds,
            "measured CPU profile did not favor the normalized PCM FIFO");
    }

    void TestAudioOutputFormatMappings()
    {
        const auto systemFormat = MakeFloatFormat(48'000, 2);
        struct Mapping
        {
            MusicPlayerLibrary::AudioChannelMode mode;
            std::uint16_t channels;
            std::uint32_t mask;
            std::string_view layout;
        };
        constexpr Mapping mappings[] = {
            {MusicPlayerLibrary::AudioChannelMode::Mono, 1, SPEAKER_MONO, "mono"},
            {MusicPlayerLibrary::AudioChannelMode::Stereo, 2, SPEAKER_STEREO, "stereo"},
            {MusicPlayerLibrary::AudioChannelMode::Surround51, 6, SPEAKER_5POINT1_SURROUND, "5.1(side)"},
            {MusicPlayerLibrary::AudioChannelMode::Surround71, 8, SPEAKER_7POINT1_SURROUND, "7.1"}
        };

        for (const auto& mapping : mappings)
        {
            MusicPlayerLibrary::AudioOutputFormat requested{};
            requested.requested_sample_rate = 96'000;
            requested.requested_channel_mode = mapping.mode;
            requested.requested_bit_depth = MusicPlayerLibrary::AudioBitDepth::Bit16;
            const auto resolved = MusicPlayerLibrary::ResolveAudioOutputFormat(
                requested, systemFormat);
            NATIVE_REQUIRE(resolved.sample_rate == 96'000,
                    "explicit output sample rate was not retained");
            NATIVE_REQUIRE(resolved.channel_count == mapping.channels &&
                    resolved.channel_mask == mapping.mask &&
                    resolved.ffmpeg_channel_layout == mapping.layout,
                    "explicit channel mode mapped to the wrong FFmpeg/FAudio layout");
            NATIVE_REQUIRE(resolved.sample_format == AV_SAMPLE_FMT_S16 &&
                    resolved.wave_format.Format.wBitsPerSample == 16 &&
                    resolved.wave_format.Format.nBlockAlign == mapping.channels * 2,
                    "16-bit output format was not resolved consistently");
        }

        MusicPlayerLibrary::AudioOutputFormat floatRequest{};
        floatRequest.requested_channel_mode = MusicPlayerLibrary::AudioChannelMode::Stereo;
        floatRequest.requested_bit_depth = MusicPlayerLibrary::AudioBitDepth::Bit32;
        const auto floatResolved = MusicPlayerLibrary::ResolveAudioOutputFormat(
            floatRequest, systemFormat);
        NATIVE_REQUIRE(floatResolved.sample_rate == 48'000 &&
                floatResolved.sample_format == AV_SAMPLE_FMT_FLT &&
                floatResolved.wave_format.Format.wBitsPerSample == 32 &&
                floatResolved.wave_format.Format.nBlockAlign == 8,
                "32-bit float output format was not resolved consistently");
    }

    void TestRawAudioFormatInfoMappings()
    {
        static_assert(std::is_trivially_copyable_v<
            MusicPlayerLibrary::AudioFormatInfo>);

        constexpr int Unknown = static_cast<int>(
            MusicPlayerLibrary::AudioChannelMode::Unknown);
        const MusicPlayerLibrary::AudioFormatInfo empty{};
        NATIVE_REQUIRE(empty.channel_type_id == Unknown &&
                    empty.sample_rate == 0 &&
                    empty.bit_depth == static_cast<int>(
                        MusicPlayerLibrary::AudioBitDepth::Unknown),
                "raw audio format defaults were not unknown");

        struct Mapping
        {
            MusicPlayerLibrary::AudioChannelMode mode;
            int expectedChannelTypeId;
        };
        constexpr Mapping mappings[] = {
            {MusicPlayerLibrary::AudioChannelMode::Mono, 1},
            {MusicPlayerLibrary::AudioChannelMode::Stereo, 2},
            {MusicPlayerLibrary::AudioChannelMode::Surround51, 3},
            {MusicPlayerLibrary::AudioChannelMode::Surround71, 4}
        };
        for (const auto& mapping : mappings)
        {
            MusicPlayerLibrary::AudioOutputFormat requested{};
            requested.requested_sample_rate = 96'000;
            requested.requested_channel_mode = mapping.mode;
            requested.requested_bit_depth =
                MusicPlayerLibrary::AudioBitDepth::Bit24;
            const auto resolved = MusicPlayerLibrary::ResolveAudioOutputFormat(
                requested, MakeFloatFormat());
            const auto info = MusicPlayerLibrary::GetAudioFormatInfo(resolved);
            NATIVE_REQUIRE(info.channel_type_id == mapping.expectedChannelTypeId &&
                        info.sample_rate == 96'000 && info.bit_depth == 24,
                    "resolved output was not converted to raw format metadata");
        }

        NATIVE_REQUIRE(MusicPlayerLibrary::GetAudioChannelTypeId(
                    6, SPEAKER_5POINT1) == 3 &&
                    MusicPlayerLibrary::GetAudioChannelTypeId(
                    6, SPEAKER_5POINT1_SURROUND) == 3 &&
                    MusicPlayerLibrary::GetAudioChannelTypeId(
                    8, SPEAKER_7POINT1) == 4 &&
                    MusicPlayerLibrary::GetAudioChannelTypeId(
                    8, SPEAKER_7POINT1_SURROUND) == 4,
                "standard surround layouts were assigned the wrong channel type id");
        NATIVE_REQUIRE(MusicPlayerLibrary::GetAudioChannelTypeId(4) == Unknown &&
                    MusicPlayerLibrary::GetAudioChannelTypeId(6) == Unknown &&
                    MusicPlayerLibrary::GetAudioChannelTypeId(8) == Unknown,
                "non-specific channel layouts were misclassified as surround");
    }

    void TestAudioSourceBitrateCalculation()
    {
        const double bitrate =
            MusicPlayerLibrary::CalculateAudioBitrateKBytesPerSecond(
                32'000, 2.0);
        NATIVE_REQUIRE(std::abs(bitrate - 16.0) < 1.0e-9,
                "128 kbit/s did not resolve to 16 KByte/s");
        NATIVE_REQUIRE(MusicPlayerLibrary::CalculateAudioBitrateKBytesPerSecond(
                    256'000, 0.0) == 0.0 &&
                    MusicPlayerLibrary::CalculateAudioBitrateKBytesPerSecond(
                    0, 2.0) == 0.0,
                "audio bitrate accepted an incomplete observation");
        NATIVE_REQUIRE(MusicPlayerLibrary::CalculateAudioBitrateKBytesPerSecond(
                    256'000, std::numeric_limits<double>::infinity()) == 0.0,
                "audio bitrate accepted a non-finite duration");

        MusicPlayerLibrary::AudioBitrateTracker tracker;
        NATIVE_REQUIRE(tracker.GetKBytesPerSecond() == 0.0,
                "audio bitrate tracker did not start empty");
        tracker.ObserveEncodedBytes(16'000);
        tracker.ObserveDecodedSamples(12'000, 48'000);
        tracker.ObserveDecodedSamples(12'000, 48'000);
        NATIVE_REQUIRE(std::abs(tracker.GetKBytesPerSecond() - 32.0) < 1.0e-9,
                "audio bitrate tracker did not combine multiple decoded frames");

        tracker.ObserveEncodedBytes(48'000);
        tracker.ObserveDecodedSamples(24'000, 48'000);
        NATIVE_REQUIRE(std::abs(tracker.GetKBytesPerSecond() - 64.0) < 1.0e-9,
                "audio bitrate tracker did not compute a VBR weighted average");

        tracker.ObserveEncodedBytes(0);
        tracker.ObserveDecodedSamples(1'024, 0);
        NATIVE_REQUIRE(std::abs(tracker.GetKBytesPerSecond() - 64.0) < 1.0e-9,
                "audio bitrate tracker accepted an invalid observation");
        tracker.Reset();
        NATIVE_REQUIRE(tracker.GetKBytesPerSecond() == 0.0,
                "audio bitrate tracker did not reset for a new decode epoch");
    }

    void TestAverageAudioQualityClassification()
    {
        const double thresholdBitrate =
            MusicPlayerLibrary::CalculateAverageAudioBitrateBitsPerSecond(
                20'000'000, 200.0);
        NATIVE_REQUIRE(std::abs(thresholdBitrate - 800'000.0) < 1.0e-9,
                "whole-stream average bitrate was calculated incorrectly");
        NATIVE_REQUIRE(!MusicPlayerLibrary::IsLoselessAudio(thresholdBitrate),
                "exactly 800 kbit/s was classified as loseless audio");

        const double aboveThresholdBitrate =
            MusicPlayerLibrary::CalculateAverageAudioBitrateBitsPerSecond(
                25'000'000, 200.0);
        NATIVE_REQUIRE(MusicPlayerLibrary::IsLoselessAudio(aboveThresholdBitrate),
                "audio above 800 kbit/s was not classified as loseless");
        NATIVE_REQUIRE(!MusicPlayerLibrary::IsLoselessAudio(
                    MusicPlayerLibrary::CalculateAverageAudioBitrateBitsPerSecond(
                        25'000'000, 0.0)),
                "audio with an unknown duration was classified as loseless");
        NATIVE_REQUIRE(MusicPlayerLibrary::CalculateAverageAudioBitrateBitsPerSecond(
                    25'000'000, -1.0) == 0.0 &&
                    MusicPlayerLibrary::CalculateAverageAudioBitrateBitsPerSecond(
                    25'000'000, std::numeric_limits<double>::quiet_NaN()) == 0.0 &&
                    !MusicPlayerLibrary::IsLoselessAudio(
                        std::numeric_limits<double>::infinity()),
                "invalid average bitrate inputs were accepted");

        NATIVE_REQUIRE(!MusicPlayerLibrary::IsHiResAudio(48'000) &&
                    MusicPlayerLibrary::IsHiResAudio(48'001) &&
                    MusicPlayerLibrary::IsHiResAudio(96'000),
                "Hi-Res sample-rate threshold was applied incorrectly");
    }

    void TestExplicit24BitAudioOutputFormatMapping()
    {
        MusicPlayerLibrary::AudioOutputFormat requested{};
        requested.requested_sample_rate = 96'000;
        requested.requested_channel_mode =
            MusicPlayerLibrary::AudioChannelMode::Stereo;
        requested.requested_bit_depth = MusicPlayerLibrary::AudioBitDepth::Bit24;
        const auto resolved = MusicPlayerLibrary::ResolveAudioOutputFormat(
            requested, MakeFloatFormat());
        const auto& waveFormat = resolved.wave_format;

        NATIVE_REQUIRE(resolved.sample_rate == 96'000 &&
                    resolved.channel_count == 2 &&
                    resolved.bit_depth == MusicPlayerLibrary::AudioBitDepth::Bit24,
                "explicit 24-bit output request was not retained");
        NATIVE_REQUIRE(resolved.sample_format == AV_SAMPLE_FMT_S32,
                "explicit 24-bit output did not use FFmpeg's S32 container");
        NATIVE_REQUIRE(waveFormat.Format.wFormatTag == FAUDIO_FORMAT_EXTENSIBLE &&
                    waveFormat.Format.wBitsPerSample == 32 &&
                    waveFormat.Samples.wValidBitsPerSample == 24 &&
                    waveFormat.Format.nBlockAlign == 8 &&
                    SameGuid(waveFormat.SubFormat, PcmSubFormat),
                "explicit 24-bit output did not use 24-valid-in-32 PCM");
        RequirePackedFrameLayoutMatchesFaudio(resolved, 257);
    }

    void TestSystemAudioOutputFormatMapping()
    {
        auto systemFormat = MakeFloatFormat(44'100, 6);
        systemFormat.dwChannelMask = SPEAKER_5POINT1_SURROUND;
        const auto resolved = MusicPlayerLibrary::ResolveAudioOutputFormat(
            MusicPlayerLibrary::AudioOutputFormat{}, systemFormat);
        NATIVE_REQUIRE(resolved.sample_rate == 44'100 &&
                resolved.channel_count == 6 &&
                resolved.channel_mask == SPEAKER_5POINT1_SURROUND &&
                resolved.ffmpeg_channel_layout == "5.1(side)",
                "System channel mode did not use the device format");
        NATIVE_REQUIRE(resolved.bit_depth == MusicPlayerLibrary::AudioBitDepth::Bit32 &&
                resolved.sample_format == AV_SAMPLE_FMT_FLT,
                "System bit depth did not use the float device format");
        const auto resolvedInfo = MusicPlayerLibrary::GetAudioFormatInfo(resolved);
        NATIVE_REQUIRE(resolvedInfo.channel_type_id == 3 &&
                    resolvedInfo.sample_rate == 44'100 &&
                    resolvedInfo.bit_depth == 32,
                "System request did not report the resolved device metadata");

        systemFormat.Format.wFormatTag = FAUDIO_FORMAT_EXTENSIBLE;
        systemFormat.Format.wBitsPerSample = 16;
        systemFormat.Samples.wValidBitsPerSample = 16;
        systemFormat.SubFormat = PcmSubFormat;
        const auto pcmResolved = MusicPlayerLibrary::ResolveAudioOutputFormat(
            MusicPlayerLibrary::AudioOutputFormat{}, systemFormat);
        NATIVE_REQUIRE(pcmResolved.bit_depth == MusicPlayerLibrary::AudioBitDepth::Bit16 &&
                pcmResolved.sample_format == AV_SAMPLE_FMT_S16,
                "System bit depth did not use the PCM16 device format");
        NATIVE_REQUIRE(MusicPlayerLibrary::GetAudioFormatInfo(pcmResolved).bit_depth == 16,
                "PCM16 device metadata reported the wrong bit depth");
    }

    void TestSystem24BitPcmOutputFormatMapping()
    {
        constexpr std::uint16_t SystemContainerBits[] = {24, 32};
        for (const auto containerBits : SystemContainerBits)
        {
            const auto systemFormat = MakePcm24Format(
                88'200, 2, containerBits);
            const auto resolved = MusicPlayerLibrary::ResolveAudioOutputFormat(
                MusicPlayerLibrary::AudioOutputFormat{}, systemFormat);
            const auto& waveFormat = resolved.wave_format;

            NATIVE_REQUIRE(resolved.sample_rate == 88'200 &&
                        resolved.channel_count == 2 &&
                        resolved.bit_depth == MusicPlayerLibrary::AudioBitDepth::Bit24,
                    "System PCM24 format was not detected from its valid bits");
            NATIVE_REQUIRE(resolved.sample_format == AV_SAMPLE_FMT_S32,
                    "System PCM24 format did not resolve to FFmpeg S32");
            NATIVE_REQUIRE(waveFormat.Format.wFormatTag == FAUDIO_FORMAT_EXTENSIBLE &&
                        waveFormat.Format.wBitsPerSample == 32 &&
                        waveFormat.Samples.wValidBitsPerSample == 24 &&
                        waveFormat.Format.nBlockAlign == 8 &&
                        SameGuid(waveFormat.SubFormat, PcmSubFormat),
                    "System PCM24 format did not normalize to 24-valid-in-32 PCM");
            RequirePackedFrameLayoutMatchesFaudio(resolved, 511);
        }
    }

    void TestAresampleAcceptsOutputFormats()
    {
        const auto systemFormat = MakeFloatFormat(48000, 2);
        constexpr MusicPlayerLibrary::AudioChannelMode channelModes[] = {
            MusicPlayerLibrary::AudioChannelMode::Mono,
            MusicPlayerLibrary::AudioChannelMode::Stereo,
            MusicPlayerLibrary::AudioChannelMode::Surround51,
            MusicPlayerLibrary::AudioChannelMode::Surround71
        };
        constexpr MusicPlayerLibrary::AudioBitDepth bitDepths[] = {
            MusicPlayerLibrary::AudioBitDepth::Bit16,
            MusicPlayerLibrary::AudioBitDepth::Bit24,
            MusicPlayerLibrary::AudioBitDepth::Bit32
        };

        for (const auto channelMode : channelModes)
        {
            for (const auto bitDepth : bitDepths)
            {
                MusicPlayerLibrary::AudioOutputFormat requested{};
                requested.requested_sample_rate = 48'000;
                requested.requested_channel_mode = channelMode;
                requested.requested_bit_depth = bitDepth;
                const auto output = MusicPlayerLibrary::ResolveAudioOutputFormat(
                    requested, systemFormat);
                const char* sampleFormatName = av_get_sample_fmt_name(output.sample_format);
                NATIVE_REQUIRE(sampleFormatName != nullptr,
                        "resolved output format has no FFmpeg sample-format name");

                UniqueFilterGraph graph(avfilter_graph_alloc());
                NATIVE_REQUIRE(graph != nullptr, "avfilter_graph_alloc failed");
                AVFilterContext* source = nullptr;
                AVFilterContext* resample = nullptr;
                AVFilterContext* format = nullptr;
                AVFilterContext* sink = nullptr;
                const std::string sourceArgs =
                    "sample_rate=44100:sample_fmt=fltp:channel_layout=stereo";
                NATIVE_REQUIRE(avfilter_graph_create_filter(
                    &source, avfilter_get_by_name("abuffer"), "src",
                    sourceArgs.c_str(), nullptr, graph.get()) >= 0,
                    "FFmpeg rejected the test abuffer format");
                const std::string resampleArgs = std::format(
                    "sample_rate={}:out_chlayout={}:out_sample_fmt={}",
                    output.sample_rate, output.ffmpeg_channel_layout, sampleFormatName);
                NATIVE_REQUIRE(avfilter_graph_create_filter(
                    &resample, avfilter_get_by_name("aresample"), "resample",
                    resampleArgs.c_str(), nullptr, graph.get()) >= 0,
                    "aresample rejected a resolved output format");
                const std::string formatArgs = std::format(
                    "sample_fmts={}:sample_rates={}:channel_layouts={}",
                    sampleFormatName, output.sample_rate, output.ffmpeg_channel_layout);
                NATIVE_REQUIRE(avfilter_graph_create_filter(
                    &format, avfilter_get_by_name("aformat"), "format",
                    formatArgs.c_str(), nullptr, graph.get()) >= 0,
                    "aformat rejected a resolved output format");
                NATIVE_REQUIRE(avfilter_graph_create_filter(
                    &sink, avfilter_get_by_name("abuffersink"), "sink",
                    nullptr, nullptr, graph.get()) >= 0,
                    "avfilter_graph_create_filter failed for abuffersink");
                NATIVE_REQUIRE(avfilter_link(source, 0, resample, 0) >= 0 &&
                        avfilter_link(resample, 0, format, 0) >= 0 &&
                        avfilter_link(format, 0, sink, 0) >= 0,
                        "failed to link the aresample test graph");
                NATIVE_REQUIRE(avfilter_graph_config(graph.get(), nullptr) >= 0,
                        "FFmpeg could not configure a resolved output format");
                const AVFilterLink* outputLink = sink->inputs[0];
                NATIVE_REQUIRE(outputLink->sample_rate == output.sample_rate &&
                        outputLink->ch_layout.nb_channels == output.channel_count &&
                        outputLink->format == output.sample_format,
                        "aresample graph negotiated a different output format");
            }
        }
    }

    void TestFftResamplesToLegacyFormat()
    {
        const std::array<std::int16_t, 2> mono{16384, -16384};
        const auto monoFormat = MakeResolvedOutputFormat(
            MusicPlayerLibrary::AudioChannelMode::Mono,
            MusicPlayerLibrary::AudioBitDepth::Bit16);
        MusicPlayerLibrary::FFTExecuter monoExecutor(monoFormat);
        const auto monoResult = monoExecutor.ResampleToFftFormat(
            reinterpret_cast<const std::uint8_t*>(mono.data()), 2);
        const auto monoReference = ReferenceSwrFftConversion(
            monoFormat, reinterpret_cast<const std::uint8_t*>(mono.data()), 2);
        NATIVE_REQUIRE(monoResult == monoReference,
                "mono FFT conversion did not produce reference S16 stereo PCM");
        NATIVE_REQUIRE(monoResult.size() == 2 * 2 * sizeof(std::int16_t),
                "mono FFT conversion returned the wrong byte count");
        std::array<std::int16_t, 4> monoPcm{};
        std::memcpy(monoPcm.data(), monoResult.data(), monoResult.size());
        NATIVE_REQUIRE(monoPcm[0] == monoPcm[1] && monoPcm[0] > 0 &&
                    monoPcm[2] == monoPcm[3] && monoPcm[2] < 0,
                "mono FFT conversion did not produce matching stereo channels");

        constexpr int InputFrames = 1'024;
        std::vector<float> stereo(static_cast<std::size_t>(InputFrames) * 2);
        for (int frame = 0; frame < InputFrames; ++frame)
        {
            stereo[static_cast<std::size_t>(frame) * 2] =
                0.5f * std::sin(2.0f * std::numbers::pi_v<float> * 440.0f *
                                static_cast<float>(frame) / 44'100.0f);
            stereo[static_cast<std::size_t>(frame) * 2 + 1] =
                0.25f * std::cos(2.0f * std::numbers::pi_v<float> * 880.0f *
                                 static_cast<float>(frame) / 44'100.0f);
        }
        const auto stereoFormat = MakeResolvedOutputFormat(
            MusicPlayerLibrary::AudioChannelMode::Stereo,
            MusicPlayerLibrary::AudioBitDepth::Bit32,
            44'100);
        MusicPlayerLibrary::FFTExecuter stereoExecutor(stereoFormat);
        const auto stereoResult = stereoExecutor.ResampleToFftFormat(
            reinterpret_cast<const std::uint8_t*>(stereo.data()), InputFrames);
        const auto stereoReference = ReferenceSwrFftConversion(
            stereoFormat, reinterpret_cast<const std::uint8_t*>(stereo.data()),
            InputFrames);
        NATIVE_REQUIRE(stereoResult == stereoReference,
                "44.1 kHz float FFT conversion differed from 48 kHz S16 stereo reference");
        NATIVE_REQUIRE(!stereoResult.empty() &&
                    stereoResult.size() % (2 * sizeof(std::int16_t)) == 0,
                "FFT conversion did not return packed S16 stereo frames");
    }

    void Test24BitFifoAndFftFrameSizing()
    {
        constexpr int FrameCount = 4;
        const auto format = MakeResolvedOutputFormat(
            MusicPlayerLibrary::AudioChannelMode::Stereo,
            MusicPlayerLibrary::AudioBitDepth::Bit24,
            48'000);
        RequirePackedFrameLayoutMatchesFaudio(format, FrameCount);

        const std::array<std::int32_t, FrameCount * 2> input{
            0x40000000, -0x40000000,
            0x20000000, -0x20000000,
            0x10000000, -0x10000000,
            0x08000000, -0x08000000};
        NATIVE_REQUIRE(sizeof(input) == static_cast<std::size_t>(
                    FrameCount * format.wave_format.Format.nBlockAlign),
                "PCM24 test storage does not contain whole FAudio frames");

        UniqueAudioFifo fifo(av_audio_fifo_alloc(
            format.sample_format, format.channel_count, FrameCount));
        NATIVE_REQUIRE(fifo != nullptr, "failed to allocate the PCM24 audio FIFO");
        void* writePlanes[] = {const_cast<std::int32_t*>(input.data())};
        NATIVE_REQUIRE(av_audio_fifo_write(fifo.get(), writePlanes, FrameCount) == FrameCount &&
                    av_audio_fifo_size(fifo.get()) == FrameCount,
                "PCM24 FIFO did not preserve the input frame count");

        std::array<std::int32_t, FrameCount * 2> fifoOutput{};
        void* readPlanes[] = {fifoOutput.data()};
        NATIVE_REQUIRE(av_audio_fifo_read(fifo.get(), readPlanes, FrameCount) == FrameCount &&
                    fifoOutput == input,
                "PCM24 FIFO round-trip changed S32 container samples");

        MusicPlayerLibrary::FFTExecuter executor(format);
        const auto* fifoBytes = reinterpret_cast<const std::uint8_t*>(
            fifoOutput.data());
        const auto fftPcm = executor.ResampleToFftFormat(fifoBytes, FrameCount);
        const auto reference = ReferenceSwrFftConversion(
            format, fifoBytes, FrameCount);
        NATIVE_REQUIRE(fftPcm == reference,
                "PCM24 FFT conversion differed from the S32 swresample reference");
        NATIVE_REQUIRE(fftPcm.size() ==
                    static_cast<std::size_t>(FrameCount * 2 * sizeof(std::int16_t)),
                "PCM24 FFT conversion returned the wrong frame byte count");

        std::array<std::int16_t, FrameCount * 2> fftSamples{};
        std::memcpy(fftSamples.data(), fftPcm.data(), fftPcm.size());
        NATIVE_REQUIRE(fftSamples[0] > 0 && fftSamples[1] < 0 &&
                    fftSamples[2] > 0 && fftSamples[3] < 0,
                "PCM24 FFT conversion did not preserve stereo sample framing");
    }

    void TestFftSurroundResampleMatchesSwresample()
    {
        const auto verifyFormat = [](MusicPlayerLibrary::AudioChannelMode channelMode,
                                     std::span<const float> input)
        {
            const auto format = MakeResolvedOutputFormat(
                channelMode, MusicPlayerLibrary::AudioBitDepth::Bit32);
            NATIVE_REQUIRE(input.size() % format.channel_count == 0,
                    "surround FFT test input is not frame aligned");
            const int frameCount = static_cast<int>(input.size() / format.channel_count);
            MusicPlayerLibrary::FFTExecuter executor(format);
            const auto* inputBytes = reinterpret_cast<const std::uint8_t*>(input.data());
            const auto actual = executor.ResampleToFftFormat(inputBytes, frameCount);
            const auto expected = ReferenceSwrFftConversion(format, inputBytes, frameCount);
            NATIVE_REQUIRE(actual == expected,
                    "surround FFT conversion differed from S16 stereo libswresample reference");
        };

        const std::array<float, 12> fiveOne{
            0.8f, 0.4f, 0.6f, 1.0f, 0.2f, 0.3f,
            -0.3f, 0.7f, 0.1f, 0.5f, -0.6f, 0.9f};
        verifyFormat(MusicPlayerLibrary::AudioChannelMode::Surround51, fiveOne);

        const std::array<float, 16> sevenOne{
            0.8f, 0.4f, 0.6f, 1.0f, 0.2f, 0.3f, 0.5f, 0.7f,
            -0.3f, 0.7f, 0.1f, 0.5f, -0.6f, 0.9f, -0.2f, 0.4f};
        verifyFormat(MusicPlayerLibrary::AudioChannelMode::Surround71, sevenOne);
    }

    void TestFftLegacyCoreBehavior()
    {
        const auto format = MakeResolvedOutputFormat(
            MusicPlayerLibrary::AudioChannelMode::Stereo,
            MusicPlayerLibrary::AudioBitDepth::Bit16,
            48'000);
        FFTExecuterProbe executor(format);

        NATIVE_REQUIRE(executor.FrameCount() == 2'048,
                "legacy FFT frame size is no longer fixed at 2048");
        NATIVE_REQUIRE(executor.SampleRate() == 48'000,
                "legacy FFT sample rate is no longer fixed at 48 kHz");

        constexpr std::array<std::size_t, 33> ExpectedBoundaries{
            0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
            11, 12, 14, 17, 21, 26, 33, 41, 51, 63,
            79, 98, 122, 151, 188, 233, 289, 359, 446,
            554, 687, 853
        };
        const auto boundaries = executor.Boundaries();
        NATIVE_REQUIRE(boundaries.size() == ExpectedBoundaries.size() &&
                    std::equal(boundaries.begin(), boundaries.end(),
                               ExpectedBoundaries.begin()),
                "legacy logarithmic FFT band boundaries changed");

        std::vector<std::int16_t> fullScalePcm(executor.FrameCount() * 2, 32'767);
        std::vector<std::uint8_t> fullScaleBytes(executor.FrameByteCount());
        std::memcpy(
            fullScaleBytes.data(), fullScalePcm.data(), fullScaleBytes.size());
        const auto windowed = executor.Window(fullScaleBytes);
        NATIVE_REQUIRE(windowed.size() == executor.FrameCount(),
                "legacy FFT window returned the wrong frame count");
        RequireClose(0.0f, static_cast<float>(windowed.front()), 1.0e-7f,
                     "legacy FFT window no longer begins at zero");
        RequireClose(1.07668f,
                     static_cast<float>(windowed[executor.FrameCount() / 2]),
                     2.0e-4f,
                     "legacy FFT window coefficient changed");
        RequireClose(0.0f, static_cast<float>(windowed.back()), 1.0e-7f,
                     "legacy FFT window no longer ends at zero");

        const auto analyzeBins = [&executor](std::span<const int> bins,
                                              float amplitude)
        {
            std::vector<std::int16_t> pcm(executor.FrameCount() * 2);
            for (std::size_t frame = 0; frame < executor.FrameCount(); ++frame)
            {
                double sample = 0.0;
                for (const int bin : bins)
                {
                    sample += amplitude * std::sin(
                        2.0 * std::numbers::pi * static_cast<double>(bin) *
                        static_cast<double>(frame) /
                        static_cast<double>(executor.FrameCount()));
                }
                const auto quantized = static_cast<std::int16_t>(
                    std::lround(std::clamp(sample, -1.0, 1.0) * 32'767.0));
                pcm[frame * 2] = quantized;
                pcm[frame * 2 + 1] = quantized;
            }
            std::vector<std::uint8_t> bytes(executor.FrameByteCount());
            std::memcpy(bytes.data(), pcm.data(), bytes.size());
            const auto segments = executor.Analyze(bytes);
            return *std::max_element(segments.begin(), segments.end());
        };

        constexpr std::array<int, 1> MidTone{96};
        constexpr std::array<int, 1> HighTone{300};
        constexpr std::array<int, 2> TwoHighTones{300, 340};
        const float ordinaryPeak = analyzeBins(MidTone, 0.1f);
        const float loudPeak = analyzeBins(MidTone, 0.5f);
        const float highPeak = analyzeBins(HighTone, 0.1f);
        const float twoHighPeak = analyzeBins(TwoHighTones, 0.1f);
        NATIVE_REQUIRE(ordinaryPeak > 0.68f && ordinaryPeak < 0.74f,
                "legacy raw-magnitude dB scaling changed");
        NATIVE_REQUIRE(loudPeak > 0.99f,
                "legacy FFT no longer clamps a loud mid-frequency tone");
        NATIVE_REQUIRE(highPeak < ordinaryPeak * 0.9f,
                "legacy high-frequency attenuation changed");
        NATIVE_REQUIRE(twoHighPeak < highPeak + 0.03f,
                "legacy FFT bands no longer use peak-bin aggregation");
    }

#define NATIVE_TEST_CASE(suite, name, function) \
    TEST_CASE(name * doctest::test_suite(suite)) { function(); }

    NATIVE_TEST_CASE("eq", "zero dB identity", TestZeroDbIdentity)
    NATIVE_TEST_CASE("eq", "1 kHz +6 dB", TestOneKilohertzPlusSixDb)
    NATIVE_TEST_CASE("eq", "Nyquist and configured rates", TestNyquistAndConfiguredRates)
    NATIVE_TEST_CASE("limiter", "configured look-ahead delay", TestLimiterDelayAtConfiguredRates)
    NATIVE_TEST_CASE("limiter", "linked stereo ceiling", TestLimiterLinksStereoAtCeiling)
    NATIVE_TEST_CASE("limiter", "no make-up gain", TestLimiterDoesNotApplyMakeupGain)
    NATIVE_TEST_CASE("limiter", "linear release", TestLimiterReleaseIsLinear)
    NATIVE_TEST_CASE("limiter", "silent tail drain", TestLimiterSilentInputDrainsTail)
    NATIVE_TEST_CASE("limiter", "reset generation drops tail", TestLimiterResetGenerationDropsTail)
    NATIVE_TEST_CASE("limiter", "chunk invariance", TestLimiterIsChunkInvariant)
    NATIVE_TEST_CASE("fapo", "factory and registration", TestFapoFactoryAndRegistration)
    NATIVE_TEST_CASE("fapo", "extensible formats advertised", TestFapoAdvertisesExtensibleFormats)
    NATIVE_TEST_CASE("fapo", "in-place matches out-of-place", TestFapoInPlaceMatchesOutOfPlace)
    NATIVE_TEST_CASE("fapo", "disabled valid pass-through", TestFapoDisabledValidPassThrough)
    NATIVE_TEST_CASE("fapo", "unsafe process buffers rejected", TestFapoRejectsUnsafeProcessBuffers)
    NATIVE_TEST_CASE("fapo", "silent input is never read", TestFapoSilentInputNeverReadsBuffer)
    NATIVE_TEST_CASE("fapo", "invalid parameter blocks rejected", TestFapoRejectsInvalidParameterBlocks)
    NATIVE_TEST_CASE("fapo", "GetParameters destination validated", TestFapoGetParametersValidatesDestination)
    NATIVE_TEST_CASE("fapo", "invalid lock formats rejected", TestFapoLockRejectsInvalidFormats)
    NATIVE_TEST_CASE("fapo", "silent input drains tail", TestFapoSilentInputDrainsTail)
    NATIVE_TEST_CASE("format", "explicit mappings", TestAudioOutputFormatMappings)
    NATIVE_TEST_CASE("format", "raw metadata mappings", TestRawAudioFormatInfoMappings)
    NATIVE_TEST_CASE("format", "source bitrate calculation", TestAudioSourceBitrateCalculation)
    NATIVE_TEST_CASE("format", "average bitrate quality flags", TestAverageAudioQualityClassification)
    NATIVE_TEST_CASE("format", "explicit PCM24 mapping and frame bytes", TestExplicit24BitAudioOutputFormatMapping)
    NATIVE_TEST_CASE("format", "System device mapping", TestSystemAudioOutputFormatMapping)
    NATIVE_TEST_CASE("format", "System PCM24 mapping and frame bytes", TestSystem24BitPcmOutputFormatMapping)
    NATIVE_TEST_CASE("format", "aresample accepts mappings", TestAresampleAcceptsOutputFormats)
    NATIVE_TEST_CASE("fft", "fixed 48 kHz S16 stereo resampling", TestFftResamplesToLegacyFormat)
    NATIVE_TEST_CASE("fft", "PCM24 FIFO and resampling frame bytes", Test24BitFifoAndFftFrameSizing)
    NATIVE_TEST_CASE("fft", "surround resampling", TestFftSurroundResampleMatchesSwresample)
    NATIVE_TEST_CASE("fft", "legacy core behavior", TestFftLegacyCoreBehavior)
    NATIVE_TEST_CASE("buffering", "CPU profiles favor normalized FIFO", TestAudioPipelineBufferingProfiles)

#undef NATIVE_TEST_CASE
#undef NATIVE_REQUIRE
}
