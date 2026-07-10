#pragma once

#include "EqualizerDsp.h"

#include <FAPO.h>

#include <cstdint>

namespace MusicPlayerLibrary::AudioDsp
{
    [[nodiscard]] std::uint32_t CreateEqualizerFapo(
        const EqualizerDspSnapshot& initial,
        const LimiterConfig& limiter,
        FAPO** effect) noexcept;
}
