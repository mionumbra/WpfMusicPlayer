#include "../MusicPlayerLibrary/EqualizerDsp.h"
#include "../MusicPlayerLibrary/FapoEqualizer.h"

#include <FAPOBase.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    using namespace MusicPlayerLibrary::AudioDsp;

    constexpr FAudioGUID PcmSubFormat{
        0x00000001, 0x0000, 0x0010,
        {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}
    };
    constexpr FAudioGUID IeeeFloatSubFormat{
        0x00000003, 0x0000, 0x0010,
        {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}
    };

    void Require(bool condition, std::string_view message)
    {
        if (!condition)
            throw std::runtime_error(std::string(message));
    }

    void RequireClose(float expected, float actual, float tolerance,
                      std::string_view message)
    {
        if (!std::isfinite(actual) || std::abs(expected - actual) > tolerance)
            throw std::runtime_error(std::string(message));
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

    UniqueFapo MakeFapo(
        const EqualizerDspSnapshot& initial,
        const LimiterConfig& limiter)
    {
        FAPO* effect = nullptr;
        Require(CreateEqualizerFapo(initial, limiter, &effect) == FAUDIO_OK,
                "CreateEqualizerFapo failed");
        Require(effect != nullptr, "CreateEqualizerFapo returned a null effect");
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
        Require(dsp.Prepare(sampleRate, 2, frameCount, limiter),
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
        Require(dsp.Prepare(sampleRate, channelCount, frameCount, limiter),
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

        Require(dsp.Process(snapshot, input.data(), output.data(), FrameCount, false),
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

        Require(dsp.Process(snapshot, input.data(), output.data(), FrameCount, false),
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
                Require(std::isfinite(coefficients.b0) &&
                        std::isfinite(coefficients.b1) &&
                        std::isfinite(coefficients.b2) &&
                        std::isfinite(coefficients.a1) &&
                        std::isfinite(coefficients.a2),
                        "configured-rate coefficients must remain finite");
            }

            if (sampleRate == 16'000)
            {
                const auto& coefficients = snapshot.bands[9];
                Require(coefficients.b0 == 1.0f && coefficients.b1 == 0.0f &&
                        coefficients.b2 == 0.0f && coefficients.a1 == 0.0f &&
                        coefficients.a2 == 0.0f,
                        "16 kHz band must be identity at a 16 kHz sample rate");
                Require((snapshot.enabled_mask & (1u << 9)) == 0,
                        "Nyquist-rejected 16 kHz band must not be enabled");
            }

            const auto input = StereoSine(sampleRate, 1'000.0f, 0.1f, FrameCount);
            std::vector<float> output(input.size());
            auto dsp = MakePreparedDsp(sampleRate, FrameCount);
            Require(dsp.Process(snapshot, input.data(), output.data(), FrameCount, false),
                    "EqualizerDsp::Process rejected a configured sample rate");
            Require(std::all_of(output.begin(), output.end(),
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

            Require(dsp.GetLimiterDelayFrames() == testCase.delay_frames,
                    "limiter reported the wrong look-ahead delay");
            Require(dsp.Process(snapshot, input.data(), output.data(), frameCount, false),
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
        Require(dsp.Process(snapshot, input.data(), output.data(), FrameCount, false),
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
        Require(dsp.Process(snapshot, input.data(), output.data(), FrameCount, false),
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
        Require(dsp.Process(snapshot, input.data(), output.data(), FrameCount, false),
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

        Require(dsp.Process(snapshot, impulse.data(), initialOutput.data(), 1, false),
                "limiter rejected queued tail input");
        Require(dsp.HasTail(), "limiter did not report a queued tail");

        std::vector<float> drained(static_cast<std::size_t>(DelayFrames) * 2, -1.0f);
        Require(dsp.Process(snapshot, nullptr, drained.data(), DelayFrames, true),
                "silent processing did not return the queued impulse");
        RequireClose(0.5f, drained[(static_cast<std::size_t>(DelayFrames) - 1) * 2],
                     1.0e-6f, "silent processing did not drain the queued impulse");
        RequireClose(0.5f, drained[(static_cast<std::size_t>(DelayFrames) - 1) * 2 + 1],
                     1.0e-6f, "silent processing did not drain the queued impulse");

        std::array<float, 2> finalOutput{1.0f, 1.0f};
        Require(!dsp.Process(snapshot, nullptr, finalOutput.data(), 1, true),
                "silent processing reported a tail after it drained");
        RequireClose(0.0f, finalOutput[0], 1.0e-7f,
                     "drained limiter emitted a nonzero sample");
        RequireClose(0.0f, finalOutput[1], 1.0e-7f,
                     "drained limiter emitted a nonzero sample");
        Require(!dsp.HasTail(), "limiter tail did not clear after silent draining");
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
        Require(dsp.Process(firstSnapshot, impulse.data(), initialOutput.data(), 1, false),
                "limiter rejected pre-reset input");
        Require(dsp.HasTail(), "limiter did not queue pre-reset history");

        std::vector<float> output(static_cast<std::size_t>(DelayFrames + 1) * 2, 1.0f);
        Require(!dsp.Process(resetSnapshot, nullptr, output.data(), DelayFrames + 1, true),
                "reset limiter reported discarded history as a tail");
        Require(std::all_of(output.begin(), output.end(),
                            [](float sample) { return sample == 0.0f; }),
                "reset generation did not discard queued limiter history");
        Require(!dsp.HasTail(), "reset generation left limiter tail state behind");
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
        Require(wholeDsp.Process(snapshot, input.data(), oneBlock.data(), FrameCount, false),
                "limiter rejected one-block input");

        std::vector<float> chunked = input;
        auto chunkedDsp = MakePreparedLimiter(SampleRate, FrameCount);
        for (std::uint32_t offset = 0; offset < FrameCount; offset += ChunkFrames)
        {
            const std::uint32_t count = (std::min)(ChunkFrames, FrameCount - offset);
            float* const block = chunked.data() + static_cast<std::size_t>(offset) * 2;
            Require(chunkedDsp.Process(snapshot, block, block, count, false),
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

        Require(effect->AddRef(effect.get()) == 2,
                "factory reference count was not one");
        Require(effect->Release(effect.get()) == 1,
                "factory reference count did not return to one");

        FAPORegistrationProperties* properties = nullptr;
        Require(effect->GetRegistrationProperties(
                    effect.get(), &properties) == FAUDIO_OK &&
                properties != nullptr,
                "FAPO registration properties were unavailable");
        constexpr std::uint32_t RequiredFlags =
            FAPO_FLAG_CHANNELS_MUST_MATCH |
            FAPO_FLAG_FRAMERATE_MUST_MATCH |
            FAPO_FLAG_BITSPERSAMPLE_MUST_MATCH |
            FAPO_FLAG_BUFFERCOUNT_MUST_MATCH |
            FAPO_FLAG_INPLACE_SUPPORTED;
        Require(properties->Flags == RequiredFlags,
                "FAPO registration flags were incorrect");
        Require((properties->Flags & FAPO_FLAG_INPLACE_REQUIRED) == 0,
                "FAPO incorrectly required in-place processing");
        reinterpret_cast<FAPOBase*>(effect.get())->pFree(properties);
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
        Require(LockFapo(inPlaceEffect.get(), format, format, FrameCount) == FAUDIO_OK,
                "in-place FAPO lock failed");
        Require(LockFapo(outOfPlaceEffect.get(), format, format, FrameCount) == FAUDIO_OK,
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
        Require(inPlaceResult.BufferFlags == FAPO_BUFFER_VALID &&
                outOfPlaceResult.BufferFlags == FAPO_BUFFER_VALID &&
                inPlaceResult.ValidFrameCount == FrameCount &&
                outOfPlaceResult.ValidFrameCount == FrameCount,
                "FAPO did not report the complete valid output");
        for (std::size_t index = 0; index < inPlace.size(); ++index)
        {
            RequireClose(inPlace[index], outOfPlace[index], 1.0e-6f,
                         "in-place and out-of-place FAPO output differed");
        }
        Require(std::any_of(
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
        Require(LockFapo(effect.get(), format, format, FrameCount) == FAUDIO_OK,
                "disabled pass-through FAPO lock failed");

        auto input = StereoSine(SampleRate, 777.0f, 0.37f, FrameCount);
        std::vector<float> output(
            input.size(), std::numeric_limits<float>::quiet_NaN());
        const auto result = ProcessFapo(
            effect.get(), input.data(), FAPO_BUFFER_VALID,
            output.data(), FrameCount, false);
        Require(result.BufferFlags == FAPO_BUFFER_VALID &&
                result.ValidFrameCount == FrameCount,
                "disabled pass-through did not preserve valid metadata");
        Require(output == input,
                "disabled out-of-place FAPO was not an exact pass-through");
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
            Require(LockFapo(effect.get(), format, format, FrameCount) == FAUDIO_OK,
                    "silent-input FAPO lock failed");
            std::vector<float> output(
                nanInput.size(), std::numeric_limits<float>::quiet_NaN());
            const auto result = ProcessFapo(
                effect.get(), nanInput.data(), FAPO_BUFFER_SILENT,
                output.data(), FrameCount, enabled);
            Require(result.BufferFlags == FAPO_BUFFER_SILENT &&
                    result.ValidFrameCount == FrameCount,
                    "drained silent input was not reported silent");
            Require(std::all_of(
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
            Require(LockFapo(effect.get(), format, format, FrameCount) == FAUDIO_OK,
                    "parameter-validation FAPO lock failed");
            std::vector<float> output(
                input.size(), std::numeric_limits<float>::quiet_NaN());
            const auto result = ProcessFapo(
                effect.get(), const_cast<float*>(input.data()), FAPO_BUFFER_VALID,
                output.data(), FrameCount, true);
            Require(result.BufferFlags == FAPO_BUFFER_VALID && output == input,
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

    void TestFapoLockRejectsInvalidFormats()
    {
        constexpr std::uint32_t SampleRate = 48'000;
        const auto snapshot = CompileEqualizerSnapshot(
            MakeDefaultTenBandConfig(), SampleRate, 27);
        auto effect = MakeFapo(snapshot, LimiterConfig{});
        const auto valid = MakeFloatFormat(SampleRate);

        Require(LockFapo(effect.get(), valid, valid, 128, 0, 1) != FAUDIO_OK,
                "FAPO accepted zero input buffers");
        Require(LockFapo(effect.get(), valid, valid, 128, 1, 2) != FAUDIO_OK,
                "FAPO accepted two output buffers");

        auto mismatchedChannels = valid;
        mismatchedChannels.Format.nChannels = 1;
        mismatchedChannels.Format.nBlockAlign = sizeof(float);
        mismatchedChannels.Format.nAvgBytesPerSec = SampleRate * sizeof(float);
        mismatchedChannels.dwChannelMask = 0;
        Require(LockFapo(effect.get(), valid, mismatchedChannels) != FAUDIO_OK,
                "FAPO accepted mismatched channel formats");

        auto legacyFloat = valid;
        legacyFloat.Format.wFormatTag = FAUDIO_FORMAT_IEEE_FLOAT;
        legacyFloat.Format.cbSize = 0;
        Require(LockFapo(effect.get(), legacyFloat, legacyFloat) != FAUDIO_OK,
                "FAPO accepted a non-extensible float format");

        auto pcm = valid;
        pcm.SubFormat = PcmSubFormat;
        Require(LockFapo(effect.get(), pcm, pcm) != FAUDIO_OK,
                "FAPO accepted a non-float32 extensible format");

        auto mismatchedRate = valid;
        mismatchedRate.Format.nSamplesPerSec = 44'100;
        mismatchedRate.Format.nAvgBytesPerSec =
            mismatchedRate.Format.nBlockAlign * 44'100;
        Require(LockFapo(effect.get(), valid, mismatchedRate) != FAUDIO_OK,
                "FAPO accepted mismatched sample rates");

        auto mismatchedMask = valid;
        mismatchedMask.dwChannelMask = 0;
        Require(LockFapo(effect.get(), valid, mismatchedMask) != FAUDIO_OK,
                "FAPO accepted nonmatching extensible formats");
        Require(LockFapo(effect.get(), valid, valid, 128, 1, 1, 256) != FAUDIO_OK,
                "FAPO accepted mismatched maximum frame counts");

        Require(LockFapo(effect.get(), valid, valid) == FAUDIO_OK,
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
        Require(LockFapo(effect.get(), format, format, DelayFrames) == FAUDIO_OK,
                "tail-drain FAPO lock failed");

        std::array<float, 2> impulse{0.5f, 0.5f};
        std::array<float, 2> initialOutput{
            std::numeric_limits<float>::quiet_NaN(),
            std::numeric_limits<float>::quiet_NaN()};
        const auto initialResult = ProcessFapo(
            effect.get(), impulse.data(), FAPO_BUFFER_VALID,
            initialOutput.data(), 1, true);
        Require(initialResult.BufferFlags == FAPO_BUFFER_VALID,
                "valid input did not keep FAPO output valid");

        std::vector<float> silentInput(
            static_cast<std::size_t>(DelayFrames) * 2,
            std::numeric_limits<float>::quiet_NaN());
        std::vector<float> drained(
            silentInput.size(), std::numeric_limits<float>::quiet_NaN());
        const auto drainResult = ProcessFapo(
            effect.get(), silentInput.data(), FAPO_BUFFER_SILENT,
            drained.data(), DelayFrames, true);
        Require(drainResult.BufferFlags == FAPO_BUFFER_VALID,
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
        Require(finalResult.BufferFlags == FAPO_BUFFER_SILENT &&
                finalOutput[0] == 0.0f && finalOutput[1] == 0.0f,
                "FAPO did not become silent after its tail ended");
        effect->UnlockForProcess(effect.get());
    }

    struct TestCase { const char* group; const char* name; void (*run)(); };
    const TestCase Tests[] = {
        {"eq", "zero dB identity", TestZeroDbIdentity},
        {"eq", "1 kHz +6 dB", TestOneKilohertzPlusSixDb},
        {"eq", "Nyquist and configured rates", TestNyquistAndConfiguredRates},
        {"limiter", "configured look-ahead delay", TestLimiterDelayAtConfiguredRates},
        {"limiter", "linked stereo ceiling", TestLimiterLinksStereoAtCeiling},
        {"limiter", "no make-up gain", TestLimiterDoesNotApplyMakeupGain},
        {"limiter", "linear release", TestLimiterReleaseIsLinear},
        {"limiter", "silent tail drain", TestLimiterSilentInputDrainsTail},
        {"limiter", "reset generation drops tail", TestLimiterResetGenerationDropsTail},
        {"limiter", "chunk invariance", TestLimiterIsChunkInvariant},
        {"fapo", "factory and registration", TestFapoFactoryAndRegistration},
        {"fapo", "in-place matches out-of-place", TestFapoInPlaceMatchesOutOfPlace},
        {"fapo", "disabled valid pass-through", TestFapoDisabledValidPassThrough},
        {"fapo", "silent input is never read", TestFapoSilentInputNeverReadsBuffer},
        {"fapo", "invalid parameter blocks rejected", TestFapoRejectsInvalidParameterBlocks},
        {"fapo", "invalid lock formats rejected", TestFapoLockRejectsInvalidFormats},
        {"fapo", "silent input drains tail", TestFapoSilentInputDrainsTail}
    };
}

int main(int argc, char** argv)
{
    const std::string_view selected = argc > 1 ? argv[1] : "all";
    int failed = 0;
    for (const auto& test : Tests)
    {
        if (selected != "all" && selected != test.group)
            continue;
        try { test.run(); std::cout << "PASS " << test.name << '\n'; }
        catch (const std::exception& error)
        {
            ++failed;
            std::cerr << "FAIL " << test.name << ": " << error.what() << '\n';
        }
    }
    return failed == 0 ? 0 : 1;
}
