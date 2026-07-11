// SPDX-License-Identifier: MIT

#include "pch.h"
#include "Core/NativeTraceRedirect.h"
#include "Managed/ManagedLogWriter.h"

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
        auto args = gcnew array<System::Object^>(1) { managedLog };
        logMethod->Invoke(logger, args);
    }
}

// 保留此入口初始化全局日志桥的作用，和旧版本语义匹配。
void MusicPlayerLibrary::NativeTraceRedirectManager::Init(System::Object^ logger)
{
    reflectedNativeTraceRedirectBridge = logger;
    NativeTraceRedirect::InitNativeTraceRedirect();
}

System::Object^ MusicPlayerLibrary::NativeTraceRedirectManager::GetNativeTraceRedirectBridge()
{
    return reflectedNativeTraceRedirectBridge;
}

