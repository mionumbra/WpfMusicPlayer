// SPDX-License-Identifier: MIT

#include "pch.h"
#include "NativeTraceRedirect.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

NativeTraceRedirect* NativeTraceRedirect::global_trace_redirector;


NativeTraceRedirect::NativeTraceRedirect(System::Object^ loggerObj)
    : logger(loggerObj)
    , enable_redirect(true)
    , timestamp_enable(true)
    , info_enable(true)
{
}

NativeTraceRedirect::~NativeTraceRedirect()
{
}

void NativeTraceRedirect::Enable()
{
    if (!System::Object::ReferenceEquals(logger, nullptr))
    {
        enable_redirect = true;
    }
}

void NativeTraceRedirect::Disable()
{
    enable_redirect = false;
}

void NativeTraceRedirect::flush_stream()
{
}

std::string NativeTraceRedirect::query_time_stamp() const
{
    auto now = std::chrono::system_clock::now();
    auto now_time_t = std::chrono::system_clock::to_time_t(now);
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    struct tm timeinfo;
    localtime_s(&timeinfo, &now_time_t);

    char buffer[64];
    sprintf_s(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d.%03lld",
        timeinfo.tm_year + 1900,
        timeinfo.tm_mon + 1,
        timeinfo.tm_mday,
        timeinfo.tm_hour,
        timeinfo.tm_min,
        timeinfo.tm_sec,
        now_ms.count());

    return buffer;
}

void NativeTraceRedirect::write_log(const char* file_name_full, int line_num, const char* message)
{
    if (!enable_redirect || System::Object::ReferenceEquals(logger, nullptr) || message == nullptr)
        return;

    std::lock_guard file_mut_lock(file_mut);

    std::string log_line;

    if (timestamp_enable)
    {
        log_line += "[";
        log_line += query_time_stamp();
        log_line += "] ";
    }

    if (info_enable && file_name_full != nullptr && line_num > 0)
    {
        const char* file_name = strrchr(file_name_full, '\\');
        if (file_name == nullptr)
            file_name = strrchr(file_name_full, '/');

        if (file_name != nullptr)
            file_name++;
        else
            file_name = file_name_full;

        char file_info[256];
        snprintf(file_info, sizeof(file_info), "[%s:%d] ", file_name, line_num);
        log_line += file_info;
    }

    log_line += message;

    if (!log_line.empty() && log_line.back() == '\n')
    {
        log_line.pop_back();
    }

    System::String^ managedLog = gcnew System::String(log_line.c_str(),
        0, static_cast<int>(log_line.size()), System::Text::Encoding::UTF8);

    System::Type^ loggerType = logger->GetType();
    array<System::Type^>^ paramTypes = gcnew array<System::Type^>(1) { System::String::typeid };
    System::Reflection::MethodInfo^ logMethod = loggerType->GetMethod("LogInformation", paramTypes);

    if (logMethod != nullptr)
    {
        array<System::Object^>^ args = gcnew array<System::Object^>(1) { managedLog };
        logMethod->Invoke(logger, args);
    }
}

std::string NativeTraceRedirect::format_message_va(const wchar_t* format, va_list args)
{
    if (format == nullptr)
        return {};

    va_list args_copy;
    va_copy(args_copy, args);
    int needed = _vscwprintf(format, args_copy);
    va_end(args_copy);

    if (needed <= 0)
        return {};

    std::wstring wide(static_cast<size_t>(needed), L'\0');
    vswprintf_s(wide.data(), static_cast<size_t>(needed) + 1, format, args);

    int utf8_len = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (utf8_len <= 0)
        return {};

    std::string result(static_cast<size_t>(utf8_len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, result.data(), utf8_len, nullptr, nullptr);
    return result;
}

std::string NativeTraceRedirect::format_message_va(const char* format, va_list args)
{
    if (format == nullptr)
        return {};

    va_list args_copy;
    va_copy(args_copy, args);
    int needed = _vscprintf(format, args_copy);
    va_end(args_copy);

    if (needed < 0)
        return {};

    std::string result(static_cast<size_t>(needed), '\0');
    vsprintf_s(result.data(), static_cast<size_t>(needed) + 1, format, args);
    return result;
}

void NativeTraceRedirect::TraceEx(const char* file_name, int line_num, const wchar_t* format, ...)
{
    if (!enable_redirect || format == nullptr)
        return;

    va_list args;
    va_start(args, format);
    std::string message = format_message_va(format, args);
    va_end(args);

    write_log(file_name, line_num, message.c_str());
}

void NativeTraceRedirect::TraceEx(const char* file_name, int line_num, const char* format, ...)
{
    if (!enable_redirect || format == nullptr)
        return;

    va_list args;
    va_start(args, format);
    std::string message = format_message_va(format, args);
    va_end(args);

    write_log(file_name, line_num, message.c_str());
}

void NativeTraceRedirect::Trace(const wchar_t* format, ...)
{
    if (!enable_redirect || format == nullptr)
        return;

    va_list args;
    va_start(args, format);
    std::string message = format_message_va(format, args);
    va_end(args);

    write_log(nullptr, -1, message.c_str());
}

void NativeTraceRedirect::Trace(const char* format, ...)
{
    if (!enable_redirect || format == nullptr)
        return;

    va_list args;
    va_start(args, format);
    std::string message = format_message_va(format, args);
    va_end(args);

    write_log(nullptr, -1, message.c_str());
}

NativeTraceRedirect* NativeTraceRedirect::GetTraceRedirector()
{
    return global_trace_redirector;
}

void NativeTraceRedirect::SetTraceRedirector(NativeTraceRedirect* redirector)
{
    global_trace_redirector = redirector;
}
