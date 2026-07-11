// SPDX-License-Identifier: MIT

#pragma once

namespace MusicPlayerLibrary
{
    enum class MPL_AUDIO_PRIORITY
    {
        MPL_AUDIO_PRIORITY_VERYLOW = -2,
        MPL_AUDIO_PRIORITY_LOW,
        MPL_AUDIO_PRIORITY_NORMAL,
        MPL_AUDIO_PRIORITY_HIGH,
        MPL_AUDIO_PRIORITY_CRITICAL
    };
    
    class IAudioThreadScheduleHelper
    {
    public:
        IAudioThreadScheduleHelper() {}

        virtual ~IAudioThreadScheduleHelper() = 0;

        IAudioThreadScheduleHelper(const IAudioThreadScheduleHelper&) = delete;
        IAudioThreadScheduleHelper& operator=(const IAudioThreadScheduleHelper&) = delete;
    };
    
    class IAudioThreadSchedulerFactory
    {
    public:
        virtual ~IAudioThreadSchedulerFactory() = default;
        virtual std::unique_ptr<IAudioThreadScheduleHelper> CreateAudioThreadScheduleHelper(const wchar_t* task_name, MPL_AUDIO_PRIORITY priority, const char* worker_name) = 0;
    };
    
    IAudioThreadSchedulerFactory* GetDefaultAudioThreadSchedulerFactory();
    void SetDefaultAudioThreadSchedulerFactory(IAudioThreadSchedulerFactory* factory);
    
}
