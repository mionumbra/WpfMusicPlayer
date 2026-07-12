// SPDX-License-Identifier: MIT

// pch.h: 这是预编译标头文件。
// 下方列出的文件仅编译一次，提高了将来生成的生成性能。
// 这还将影响 IntelliSense 性能，包括代码完成和许多代码浏览功能。
// 但是，如果此处列出的文件中的任何一个在生成之间有更新，它们全部都将被重新编译。
// 请勿在此处添加要频繁更新的文件，这将使得性能优势无效。

#ifndef PCH_H
#define PCH_H

#ifndef NOMINMAX
#define NOMINMAX
#endif 

#if defined(_MSC_VER) // if uses msvc...
#define _CRT_SECURE_NO_WARNINGS // NOLINT(*-reserved-identifier)
#pragma warning (disable: 4819) // avoid msvc utf-8 warning
#endif

#define _CRTDBG_MAP_ALLOC // NOLINT(*-reserved-identifier)
#ifdef _DEBUG
#define DBG_NEW new ( _NORMAL_BLOCK , __FILE__ , __LINE__ )
// Replace _NORMAL_BLOCK with _CLIENT_BLOCK if you want the
// allocations to be of _CLIENT_BLOCK type
#else
#define DBG_NEW new
#endif

// 添加要在此处预编译的标头

#if !defined(NATIVE_TRACE_REDIRECT_ENABLED)
#define NATIVE_TRACE_REDIRECT_ENABLED
#endif
#include "Core/NativeTraceRedirect.h" // For Debug

#endif //PCH_H
