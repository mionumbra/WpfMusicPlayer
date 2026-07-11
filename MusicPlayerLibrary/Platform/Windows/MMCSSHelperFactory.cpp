// SPDX-License-Identifier: MIT

#include "pch.h"
#include "Platform/Windows/MMCSSHelperFactory.h"
#include "Platform/Windows/MMCSSHelper.h"
#include "Platform/Windows/WindowsCommon.h"

namespace MusicPlayerLibrary
{
    namespace
    {
        AVRT_PRIORITY ConvertMplPriorityToAvrtPriority(MPL_AUDIO_PRIORITY priority_in)
        {
            switch (priority_in)
            {
            case MPL_AUDIO_PRIORITY::MPL_AUDIO_PRIORITY_VERYLOW: return AVRT_PRIORITY_VERYLOW;
            case MPL_AUDIO_PRIORITY::MPL_AUDIO_PRIORITY_LOW: return AVRT_PRIORITY_LOW;
            case MPL_AUDIO_PRIORITY::MPL_AUDIO_PRIORITY_NORMAL: return AVRT_PRIORITY_NORMAL;
            case MPL_AUDIO_PRIORITY::MPL_AUDIO_PRIORITY_HIGH: return AVRT_PRIORITY_HIGH;
            case MPL_AUDIO_PRIORITY::MPL_AUDIO_PRIORITY_CRITICAL: return AVRT_PRIORITY_CRITICAL;
            default: return AVRT_PRIORITY_NORMAL;
            }
        }
    }
    
    std::unique_ptr<IAudioThreadScheduleHelper> MMCSSHelperFactory::
        CreateAudioThreadScheduleHelper(const wchar_t* task_name, MPL_AUDIO_PRIORITY priority, const char* worker_name)
    {
        return std::unique_ptr<IAudioThreadScheduleHelper>(new MMCSSHelper(task_name, ConvertMplPriorityToAvrtPriority(priority), worker_name));
    }

}