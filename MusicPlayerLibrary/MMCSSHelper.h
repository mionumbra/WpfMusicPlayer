#pragma once
#include "pch.h"

namespace MusicPlayerLibrary
{
    class MMCSSHelper final
    {
    public:
        MMCSSHelper(const wchar_t* task_name, AVRT_PRIORITY priority, const char* worker_name);

        ~MMCSSHelper();

        MMCSSHelper(const MMCSSHelper&) = delete;
        MMCSSHelper& operator=(const MMCSSHelper&) = delete;

    private:
        HANDLE handle_ = nullptr;
        const char* worker_name_ = "audio";
    };
    
}
