// SPDX-License-Identifier: MIT

#include "pch.h"


#if defined(_WIN32) || defined(_WIN64) || defined(_WINDOWS_)
#include "Platform/Windows/MMCSSHelperFactory.h"
#endif

namespace MusicPlayerLibrary
{
    static std::unique_ptr<IAudioThreadSchedulerFactory> default_factory;
    
    IAudioThreadSchedulerFactory* GetDefaultAudioThreadSchedulerFactory()
    {
        if (default_factory == nullptr)
        {
#if defined(_WIN32) || defined(_WIN64) || defined(_WINDOWS_)
            default_factory = std::make_unique<MMCSSHelperFactory>();
#endif
        }
        return default_factory.get();
    }
    
    void SetDefaultAudioThreadSchedulerFactory(IAudioThreadSchedulerFactory* factory) { default_factory.reset(factory); }
}