// SPDX-License-Identifier: MIT

#include "pch.h"
#include "Core/NativeLogWriter.h"
#if defined(_WIN32) || defined(_WIN64) || defined(__cplusplus_cli)
#include "Managed/ManagedLoggerFactory.h"
#endif

namespace MusicPlayerLibrary
{
    static std::unique_ptr<INativeLogWriterFactory> defaultNativeWriterFactory;
    
    INativeLogWriterFactory* GetNativeLogWriterFactory()
    {
        if (defaultNativeWriterFactory == nullptr)
        {
#if defined(_WIN32) || defined(_WIN64) || defined(__cplusplus_cli)
            defaultNativeWriterFactory = std::make_unique<ManagedLoggerFactory>();
#endif
        }
        return defaultNativeWriterFactory.get();
    }
    
    void SetNativeLogWriterFactory(INativeLogWriterFactory* factory)
    {
        defaultNativeWriterFactory.reset(factory);
    }
}