// SPDX-License-Identifier: MIT

#include "pch.h"
#include "Managed/ManagedLoggerFactory.h"
#include "Managed/ManagedLogWriter.h"

std::unique_ptr<MusicPlayerLibrary::INativeLogWriter>
MusicPlayerLibrary::ManagedLoggerFactory::create_native_log_writer()
{
    auto loggerBridge = NativeTraceRedirectManager::GetNativeTraceRedirectBridge();
    INativeLogWriter* managedLogWriter = new ManagedLogWriter(loggerBridge);
    return std::unique_ptr<INativeLogWriter>(managedLogWriter);
}
