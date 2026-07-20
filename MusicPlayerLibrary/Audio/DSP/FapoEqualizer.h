// SPDX-License-Identifier: MIT

#pragma once

#include "Audio/DSP/EqualizerDsp.h"
#include <FAPO.h>

namespace MusicPlayerLibrary::AudioDsp
{
    [[nodiscard]] std::uint32_t CreateEqualizerFapo(
        const EqualizerDspSnapshot& initial,
        const LimiterConfig& limiter,
        FAPO** effect) noexcept;

    // These helpers publish only atomics written after a successful Process
    // pass; they never read EqualizerDsp concurrently with the audio thread.
    [[nodiscard]] bool EqualizerFapoHasTail(FAPO* effect) noexcept;
    [[nodiscard]] std::uint64_t EqualizerFapoProcessSequence(
        FAPO* effect) noexcept;
}
