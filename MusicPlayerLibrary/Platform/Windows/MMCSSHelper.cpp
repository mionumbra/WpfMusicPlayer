// SPDX-License-Identifier: MIT

#include "pch.h"
#include "Platform/Windows/MMCSSHelper.h"
#include "Platform/Windows/WindowsCommon.h"

MusicPlayerLibrary::MMCSSHelper::MMCSSHelper(const wchar_t* task_name, AVRT_PRIORITY priority, const char* worker_name):
    worker_name_(worker_name)
{
	SetThreadExecutionState(ES_SYSTEM_REQUIRED | ES_AWAYMODE_REQUIRED);
    if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST))
    {
        NATIVE_TRACE("warn: %s thread SetThreadPriority failed, gle=%lu\n",
                     worker_name_, GetLastError());
    }

    DWORD task_index = 0;
    handle_ = AvSetMmThreadCharacteristicsW(task_name, &task_index);
    if (!handle_)
    {
        NATIVE_TRACE("warn: %s thread MMCSS registration failed, gle=%lu\n",
                     worker_name_, GetLastError());
        return;
    }

    if (!AvSetMmThreadPriority(handle_, priority))
    {
        NATIVE_TRACE("warn: %s thread AvSetMmThreadPriority failed, priority=%d, gle=%lu\n",
                     worker_name_, static_cast<int>(priority), GetLastError());
        return;
    }

    NATIVE_TRACE("info: %s thread registered with MMCSS priority=%d\n",
                 worker_name_, static_cast<int>(priority));
}

MusicPlayerLibrary::MMCSSHelper::~MMCSSHelper()
{
    if (handle_ && !AvRevertMmThreadCharacteristics(handle_))
    {
        NATIVE_TRACE("warn: %s thread MMCSS revert failed, gle=%lu\n",
                     worker_name_, GetLastError());
    }
}
