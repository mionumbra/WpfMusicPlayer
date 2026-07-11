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
}
