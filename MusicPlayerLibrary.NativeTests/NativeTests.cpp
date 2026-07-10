#include "../MusicPlayerLibrary/EqualizerDsp.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    using namespace MusicPlayerLibrary::AudioDsp;

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
        {"limiter", "chunk invariance", TestLimiterIsChunkInvariant}
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
