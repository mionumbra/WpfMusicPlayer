// SPDX-License-Identifier: MIT

#pragma once
#include <msclr/marshal_cppstd.h>

namespace MusicPlayerLibrary
{
    public ref class LocaleConverterManaged abstract sealed {
        public:
        static System::String^ GetSystemStringFromBytesManaged(array<byte>^ input);
        static bool IsUtf8CompatibleBytesManaged(array<byte>^ input);
		static System::String^ GetSystemStringFromUtf8String(const std::string& input);
    };
}