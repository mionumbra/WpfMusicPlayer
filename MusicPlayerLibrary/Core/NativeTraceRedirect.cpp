// SPDX-License-Identifier: MIT

#include "pch.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cwchar>
#include <iomanip>
#include <sstream>
#include <string>

#include "Core/NativeTraceRedirect.h"
#include "Core/LocaleConverter.h"

std::unique_ptr<NativeTraceRedirect> NativeTraceRedirect::global_trace_redirector;


NativeTraceRedirect::NativeTraceRedirect(std::unique_ptr<MusicPlayerLibrary::INativeLogWriter> logWriter)
    : enable_redirect(true)
    , timestamp_enable(true)
    , info_enable(true)
    , native_log_writer_(std::move(logWriter))
{
}

NativeTraceRedirect::~NativeTraceRedirect()
{
}

void NativeTraceRedirect::Enable()
{
    enable_redirect = true;
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

    const std::tm* timeinfo = std::localtime(&now_time_t);
    if (!timeinfo)
        return {};

    std::ostringstream timestamp;
    timestamp << std::put_time(timeinfo, "%Y-%m-%d %H:%M:%S")
        << '.' << std::setfill('0') << std::setw(3) << now_ms.count();
    return timestamp.str();
}

void NativeTraceRedirect::write_log(const char* file_name_full, int line_num, const char* message)
{
    if (!enable_redirect || !native_log_writer_ || message == nullptr)
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
    
    native_log_writer_->write_log(log_line);
}

std::string NativeTraceRedirect::format_message_va(const wchar_t* format, va_list args)
{
    if (format == nullptr)
        return {};

    std::vector<wchar_t> buffer(256);
    while (buffer.size() <= 1024 * 1024)
    {
        va_list args_copy;
        va_copy(args_copy, args);
        const int written = std::vswprintf(buffer.data(), buffer.size(), format, args_copy);
        va_end(args_copy);

        if (written >= 0 && static_cast<size_t>(written) < buffer.size())
        {
            return MusicPlayerLibrary::LocaleConverter::GetUtf8StringFromUtf16String(
                std::wstring(buffer.data(), static_cast<size_t>(written)));
        }
        buffer.resize(written > 0 ? static_cast<size_t>(written) + 1 : buffer.size() * 2);
    }
    return {};
}

std::string NativeTraceRedirect::format_message_va(const char* format, va_list args)
{
    if (format == nullptr)
        return {};

    va_list args_copy;
    va_copy(args_copy, args);
    const int needed = std::vsnprintf(nullptr, 0, format, args_copy);
    va_end(args_copy);

    if (needed < 0)
        return {};

    std::vector<char> buffer(static_cast<size_t>(needed) + 1);
    va_copy(args_copy, args);
    std::vsnprintf(buffer.data(), buffer.size(), format, args_copy);
    va_end(args_copy);
    return {buffer.data(), static_cast<size_t>(needed)};
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
    return global_trace_redirector.get();
}

void NativeTraceRedirect::SetTraceRedirector(NativeTraceRedirect* redirector)
{
    global_trace_redirector.reset(redirector);
}

void NativeTraceRedirect::InitNativeTraceRedirect()
{
    global_trace_redirector = std::make_unique<NativeTraceRedirect>(MusicPlayerLibrary::GetNativeLogWriterFactory()->create_native_log_writer());
}
