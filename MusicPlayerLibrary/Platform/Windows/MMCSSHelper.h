// SPDX-License-Identifier: MIT

#pragma once
#include "pch.h"
#include "Core/AudioThreadScheduleHelper.h"
#include "Platform/Windows/WindowsCommon.h"

namespace MusicPlayerLibrary
{
    class MMCSSHelper final: public IAudioThreadScheduleHelper
    {
    public:
        MMCSSHelper(const wchar_t* task_name, AVRT_PRIORITY priority, const char* worker_name);

        ~MMCSSHelper() override;
    private:
        HANDLE handle_ = nullptr;
        const char* worker_name_ = "audio";
    };
    
}
