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

    struct TestCase { const char* group; const char* name; void (*run)(); };
    const TestCase Tests[] = {
        {"eq", "zero dB identity", TestZeroDbIdentity},
        {"eq", "1 kHz +6 dB", TestOneKilohertzPlusSixDb},
        {"eq", "Nyquist and configured rates", TestNyquistAndConfiguredRates}
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
