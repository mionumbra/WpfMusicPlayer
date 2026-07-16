// SPDX-License-Identifier: MIT

#pragma once
#include "Core/NativeLogWriter.h"
#include <vcclr.h>

namespace MusicPlayerLibrary
{
    class ManagedLogWriter : public INativeLogWriter
    {
        gcroot<System::Object^> logger;
        gcroot<System::Reflection::MethodInfo^> logMethod;

    public:
        ManagedLogWriter(System::Object^ loggerObj);
        void write_log(const std::string& message) override;
    };

    public ref class NativeTraceRedirectManager
    {
        static Object^ reflectedNativeTraceRedirectBridge;
    internal:
        static void SetNativeTraceRedirectBridge(Object^ logger);
        static void ClearNativeTraceRedirectBridge();
    public:
        static void Init(System::Object^ logger);
        static Object^ GetNativeTraceRedirectBridge();
    };
}
