// SPDX-License-Identifier: MIT

#include "pch.h"

#include <exception>
#include <string>

#include "Core/NativeLibraryRuntime.h"
#include "Core/NativeTraceRedirect.h"
#include "Managed/ManagedLogWriter.h"
#include "Managed/MusicPlayerLibraryRuntime.h"

namespace
{
	System::InvalidOperationException^ ToManagedRuntimeException(
		const std::exception& exception)
	{
		const std::string message = exception.what();
		return gcnew System::InvalidOperationException(
			gcnew System::String(
				message.c_str(), 0, static_cast<int>(message.size()),
				System::Text::Encoding::UTF8));
	}
}

void MusicPlayerLibrary::MusicPlayerLibraryRuntime::Initialize(System::Object^ logger)
{
	if (logger == nullptr)
		throw gcnew System::ArgumentNullException("logger");

	if (NativeLibraryRuntime::IsInitialized())
		return;
	
	NativeTraceRedirectManager::SetNativeTraceRedirectBridge(logger);
	try
	{
		NativeTraceRedirect::InitNativeTraceRedirect();
		NativeLibraryRuntime::Initialize();
	}
	catch (const std::exception& exception)
	{
		NativeLibraryRuntime::Shutdown();
		NativeTraceRedirectManager::ClearNativeTraceRedirectBridge();
		throw ToManagedRuntimeException(exception);
	}
	catch (System::Exception^)
	{
		NativeLibraryRuntime::Shutdown();
		NativeTraceRedirectManager::ClearNativeTraceRedirectBridge();
		throw;
	}
	catch (...)
	{
		NativeLibraryRuntime::Shutdown();
		NativeTraceRedirectManager::ClearNativeTraceRedirectBridge();
		throw gcnew System::InvalidOperationException(
			"native library runtime initialization failed");
	}
}

void MusicPlayerLibrary::MusicPlayerLibraryRuntime::Shutdown()
{
	try
	{
		NativeLibraryRuntime::Shutdown();
	}
	finally
	{
		NativeTraceRedirectManager::ClearNativeTraceRedirectBridge();
	}
}

bool MusicPlayerLibrary::MusicPlayerLibraryRuntime::IsInitialized()
{
	return NativeLibraryRuntime::IsInitialized();
}
