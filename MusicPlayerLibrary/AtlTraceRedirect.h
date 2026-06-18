#pragma once

#include <chrono>
#include <mutex>
#include <string>
#include <vcclr.h>

class AtlTraceRedirect
{
public:
    explicit AtlTraceRedirect(System::Object^ logger);

    ~AtlTraceRedirect();

    AtlTraceRedirect(const AtlTraceRedirect&) = delete;
    AtlTraceRedirect& operator=(const AtlTraceRedirect&) = delete;

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

    static AtlTraceRedirect* GetAtlTraceRedirector();
    static void SetAtlTraceRedirector(AtlTraceRedirect*);

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

    static AtlTraceRedirect* global_atl_trace_redirector;
};

#define ATLTRACE_REDIRECT_EX(redirector, fmt, ...) \
    do { \
        if ((redirector) != nullptr) { \
            (redirector)->TraceEx(__FILE__, __LINE__, fmt, __VA_ARGS__); \
        } \
    } while(0)

#define ATLTRACE_REDIRECT(redirector, fmt, ...) \
    do { \
        if ((redirector) != nullptr) { \
            (redirector)->Trace(fmt, __VA_ARGS__); \
        } \
    } while(0)
#if defined(ATLTRACE_REDIRECT_ENABLED)
#if defined(ATLTRACE)
#undef ATLTRACE
#endif
#define ATLTRACE(fmt, ...) ATLTRACE_REDIRECT_EX(AtlTraceRedirect::GetAtlTraceRedirector(), fmt, __VA_ARGS__)
#endif
