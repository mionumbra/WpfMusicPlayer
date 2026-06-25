#pragma once

#include "pch.h"

namespace MusicPlayerLibrary 
{
	class LocaleConverterNative
	{
	public:
		static std::string GetUtf8StringFromBytesNative(const char* input, size_t size);
		static std::wstring GetUtf16StringFromUtf8String(const std::string& input);
		static std::string GetUtf8StringFromUtf16String(const std::wstring& input);
		static System::String^ GetSystemStringFromUtf8String(const std::string& input);
	};

	public ref class LocaleConverter abstract sealed {
	public:
		static System::String^ GetSystemStringFromBytes(array<byte>^ input);
	};
}
