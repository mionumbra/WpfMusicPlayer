// SPDX-License-Identifier: MIT

#include "pch.h"
#include "Core/NativeTraceRedirect.h"
#include "Managed/ManagedLogWriter.h"
#include "Managed/MusicPlayerLibraryRuntime.h"

MusicPlayerLibrary::ManagedLogWriter::ManagedLogWriter(System::Object^ loggerObj):
    logger(loggerObj)
{
    System::Type^ loggerType = logger->GetType();
    array<System::Type^>^ paramTypes = gcnew array<System::Type^>(1) { System::String::typeid };
    logMethod = loggerType->GetMethod("LogInformation", paramTypes);
}

void MusicPlayerLibrary::ManagedLogWriter::write_log(const std::string& message)
{
    auto managedLog = gcnew System::String(message.c_str(),
        0, static_cast<int>(message.size()), System::Text::Encoding::UTF8);
    if (logMethod)
    {
        try
        {
            auto args = gcnew array<System::Object^>(1) { managedLog };
            logMethod->Invoke(logger, args);
        }
        catch (System::Exception^)
        {
            // Native logging is diagnostic only. A disposed or failing CLR
            // logger must not interrupt native resource cleanup.
        }
    }
}

// Backward-compatible entry point. New callers should use
// MusicPlayerLibraryRuntime so initialization and teardown stay paired.
void MusicPlayerLibrary::NativeTraceRedirectManager::Init(System::Object^ logger)
{
    MusicPlayerLibraryRuntime::Initialize(logger);
}

void MusicPlayerLibrary::NativeTraceRedirectManager::SetNativeTraceRedirectBridge(
    System::Object^ logger)
{
    reflectedNativeTraceRedirectBridge = logger;
}

void MusicPlayerLibrary::NativeTraceRedirectManager::ClearNativeTraceRedirectBridge()
{
    reflectedNativeTraceRedirectBridge = nullptr;
}

System::Object^ MusicPlayerLibrary::NativeTraceRedirectManager::GetNativeTraceRedirectBridge()
{
    return reflectedNativeTraceRedirectBridge;
}

