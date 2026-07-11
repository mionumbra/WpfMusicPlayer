// SPDX-License-Identifier: MIT

#pragma once
#include "Core/AudioThreadScheduleHelper.h"

namespace MusicPlayerLibrary
{
    class MMCSSHelperFactory: public IAudioThreadSchedulerFactory
    {
    public:
        std::unique_ptr<IAudioThreadScheduleHelper> CreateAudioThreadScheduleHelper(const wchar_t* task_name,
            MPL_AUDIO_PRIORITY priority, const char* worker_name) override;
    };

}