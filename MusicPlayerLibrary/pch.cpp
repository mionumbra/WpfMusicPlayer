// SPDX-License-Identifier: MIT

// pch.cpp: 与预编译标头对应的源文件

#include "pch.h"

// 当使用预编译的头时，需要使用此源文件，编译才能成功。
#include <cpp-base64/base64.cpp>

// Define Windows Runtime GUIDs for System Media Transport Controls
#include <initguid.h>
#include <SystemMediaTransportControlsInterop.h>

#pragma comment(lib, "runtimeobject.lib")