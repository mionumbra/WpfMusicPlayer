// SPDX-License-Identifier: MIT

#pragma once

#include <chrono>
#include <mutex>
#include <string>
#include <vcclr.h>

class NativeTraceRedirect
{
public:
    explicit NativeTraceRedirect(System::Object^ logger);

    ~NativeTraceRedirect();

    NativeTraceRedirect(const NativeTraceRedirect&) = delete;
    NativeTraceRedirect& operator=(const NativeTraceRedirect&) = delete;

    void Enable();
    void Disable();
    [[nodiscard]] bool IsEnabled() const { return enable_redirect; }

    void flush_stream();

    void SetIncludeTimestamp(bool include) { timestamp_enable = include; }
    void SetIncludeFileInfo(bool include) { info_enable = include; }

    void TraceEx(const char* file_name, int line_num, const wchar_t* format, ...);
    void TraceEx(const char* file_name, int line_num, const char* format, ...);

    void Trace(const wchar_t* format, ...);
    void Trace(const char* format, ...);

    static NativeTraceRedirect* GetTraceRedirector();
    static void SetTraceRedirector(NativeTraceRedirect*);

private:
    [[nodiscard]] std::string query_time_stamp() const;

    void write_log(const char* file_name_full, int line_num, const char* message);

    std::string format_message_va(const wchar_t* format, va_list args);
    std::string format_message_va(const char* format, va_list args);

    gcroot<System::Object^> logger;
    bool enable_redirect;
    bool timestamp_enable;
    bool info_enable;
    std::mutex file_mut;

    static NativeTraceRedirect* global_trace_redirector;
};

#define NATIVE_TRACE_REDIRECT_EX(redirector, fmt, ...) \
    do { \
        if ((redirector) != nullptr) { \
            (redirector)->TraceEx(__FILE__, __LINE__, fmt, __VA_ARGS__); \
        } \
    } while(0)

#define NATIVE_TRACE_REDIRECT(redirector, fmt, ...) \
    do { \
        if ((redirector) != nullptr) { \
            (redirector)->Trace(fmt, __VA_ARGS__); \
        } \
    } while(0)
#if defined(NATIVE_TRACE_REDIRECT_ENABLED)
#if defined(NATIVE_TRACE)
#undef NATIVE_TRACE
#endif
#define NATIVE_TRACE(fmt, ...) NATIVE_TRACE_REDIRECT_EX(NativeTraceRedirect::GetTraceRedirector(), fmt, __VA_ARGS__)
#endif
