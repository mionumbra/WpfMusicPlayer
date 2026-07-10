#include "EqualizerDsp.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace MusicPlayerLibrary::AudioDsp
{
    namespace
    {
        [[nodiscard]] bool IsUsableBand(
            const EqualizerBandConfig& band, std::uint32_t sampleRate) noexcept
        {
            return band.enabled &&
                band.type == BiquadType::Peaking &&
                band.gain_db != 0.0f &&
                std::isfinite(band.gain_db) &&
                std::isfinite(band.frequency_hz) &&
                band.frequency_hz > 0.0f &&
                std::isfinite(band.q) &&
                band.q > 0.0f &&
                sampleRate != 0 &&
                2.0 * static_cast<double>(band.frequency_hz) < sampleRate;
        }

        [[nodiscard]] bool TryCompilePeaking(
            const EqualizerBandConfig& band,
            std::uint32_t sampleRate,
            BiquadCoefficients& coefficients) noexcept
        {
            if (!IsUsableBand(band, sampleRate))
                return false;

            const double frequency = band.frequency_hz;
            const double q = band.q;
            const double gainDb = band.gain_db;
            const double a = std::pow(10.0, gainDb / 40.0);
            const double omega = 2.0 * std::numbers::pi_v<double> *
                frequency / static_cast<double>(sampleRate);
            const double alpha = std::sin(omega) / (2.0 * q);
            const double cosine = std::cos(omega);
            const double a0 = 1.0 + alpha / a;

            const double normalizedB0 = (1.0 + alpha * a) / a0;
            const double normalizedB1 = (-2.0 * cosine) / a0;
            const double normalizedB2 = (1.0 - alpha * a) / a0;
            const double normalizedA1 = (-2.0 * cosine) / a0;
            const double normalizedA2 = (1.0 - alpha / a) / a0;

            if (!std::isfinite(normalizedB0) ||
                !std::isfinite(normalizedB1) ||
                !std::isfinite(normalizedB2) ||
                !std::isfinite(normalizedA1) ||
                !std::isfinite(normalizedA2))
            {
                return false;
            }

            coefficients.b0 = static_cast<float>(normalizedB0);
            coefficients.b1 = static_cast<float>(normalizedB1);
            coefficients.b2 = static_cast<float>(normalizedB2);
            coefficients.a1 = static_cast<float>(normalizedA1);
            coefficients.a2 = static_cast<float>(normalizedA2);
            return std::isfinite(coefficients.b0) &&
                std::isfinite(coefficients.b1) &&
                std::isfinite(coefficients.b2) &&
                std::isfinite(coefficients.a1) &&
                std::isfinite(coefficients.a2);
        }
    }

    EqualizerConfig MakeDefaultTenBandConfig() noexcept
    {
        EqualizerConfig config;
        for (std::size_t index = 0; index < EqualizerBandCount; ++index)
        {
            config.bands[index].type = BiquadType::Peaking;
            config.bands[index].frequency_hz = EqualizerBandFrequenciesHz[index];
            config.bands[index].q = 1.0f;
            config.bands[index].gain_db = 0.0f;
            config.bands[index].enabled = true;
        }
        return config;
    }

    EqualizerDspSnapshot CompileEqualizerSnapshot(
        const EqualizerConfig& config,
        std::uint32_t sampleRate,
        std::uint64_t resetGeneration) noexcept
    {
        EqualizerDspSnapshot snapshot;
        snapshot.byte_size = sizeof(EqualizerDspSnapshot);
        snapshot.reset_generation = resetGeneration;

        for (std::size_t index = 0; index < EqualizerBandCount; ++index)
        {
            BiquadCoefficients coefficients;
            if (!TryCompilePeaking(config.bands[index], sampleRate, coefficients))
                continue;

            snapshot.bands[index] = coefficients;
            snapshot.enabled_mask |= std::uint32_t{1} << index;
        }

        return snapshot;
    }

    bool EqualizerDsp::Prepare(
        std::uint32_t sampleRate,
        std::uint32_t channelCount,
        std::uint32_t maxFrameCount,
        const LimiterConfig& limiter)
    {
        if (sampleRate == 0 || channelCount == 0 || maxFrameCount == 0)
            return false;

        sample_rate_ = sampleRate;
        channel_count_ = channelCount;
        max_frame_count_ = maxFrameCount;
        limiter_ = limiter;
        biquad_states_.assign(
            static_cast<std::size_t>(channelCount) * EqualizerBandCount, {});
        applied_reset_generation_ = ~std::uint64_t{0};
        return true;
    }

    void EqualizerDsp::Reset() noexcept
    {
        std::fill(biquad_states_.begin(), biquad_states_.end(), BiquadState{});
        applied_reset_generation_ = ~std::uint64_t{0};
    }

    bool EqualizerDsp::Process(
        const EqualizerDspSnapshot& snapshot,
        const float* input,
        float* output,
        std::uint32_t frameCount,
        bool) noexcept
    {
        if (sample_rate_ == 0 || channel_count_ == 0 ||
            frameCount > max_frame_count_ ||
            (frameCount != 0 && (input == nullptr || output == nullptr)) ||
            snapshot.abi_version != EqualizerSnapshotAbiVersion ||
            snapshot.byte_size != sizeof(EqualizerDspSnapshot))
        {
            return false;
        }

        if (applied_reset_generation_ != snapshot.reset_generation)
        {
            Reset();
            applied_reset_generation_ = snapshot.reset_generation;
        }

        for (std::uint32_t channel = 0; channel < channel_count_; ++channel)
        {
            for (std::size_t band = 0; band < EqualizerBandCount; ++band)
            {
                if ((snapshot.enabled_mask & (std::uint32_t{1} << band)) == 0)
                {
                    biquad_states_[
                        static_cast<std::size_t>(channel) * EqualizerBandCount + band] = {};
                }
            }
        }

        for (std::uint32_t frame = 0; frame < frameCount; ++frame)
        {
            for (std::uint32_t channel = 0; channel < channel_count_; ++channel)
            {
                const auto sampleIndex =
                    static_cast<std::size_t>(frame) * channel_count_ + channel;
                float x = input[sampleIndex];

                for (std::size_t band = 0; band < EqualizerBandCount; ++band)
                {
                    if ((snapshot.enabled_mask & (std::uint32_t{1} << band)) == 0)
                        continue;

                    const auto& coefficients = snapshot.bands[band];
                    auto& state = biquad_states_[
                        static_cast<std::size_t>(channel) * EqualizerBandCount + band];
                    const float y = coefficients.b0 * x + state.z1;
                    state.z1 = coefficients.b1 * x - coefficients.a1 * y + state.z2;
                    state.z2 = coefficients.b2 * x - coefficients.a2 * y;
                    x = y;
                }

                output[sampleIndex] = x;
            }
        }

        return true;
    }

    bool EqualizerDsp::HasTail() const noexcept
    {
        return false;
    }

    std::uint32_t EqualizerDsp::GetLimiterDelayFrames() const noexcept
    {
        return 0;
    }
}
