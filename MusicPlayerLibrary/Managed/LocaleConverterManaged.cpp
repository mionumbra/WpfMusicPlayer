// SPDX-License-Identifier: MIT

#include "pch.h"
#include "Managed/LocaleConverterManaged.h"
#include "Core/LocaleConverter.h"

namespace MusicPlayerLibrary
{
    System::String^ LocaleConverterManaged::GetSystemStringFromBytesManaged(array<byte>^ input)
    {
        if (input == nullptr || input->Length == 0)
            return gcnew System::String(L"");

        std::vector<char> buffer(input->Length);
        for (int i = 0; i < input->Length; ++i)
            buffer[i] = input[i];

        std::string utf8 = LocaleConverter::GetUtf8StringFromBytes(buffer.data(), buffer.size());
        std::wstring s = LocaleConverter::GetUtf16StringFromUtf8String(utf8);

        while (!s.empty())
        {
            wchar_t ch = s.back();
            if ((ch >= 0x20 && ch <= 0xD7FF) ||
                (ch >= 0xE000 && ch <= 0xFFFD))
            {
                break;
            }
            // 剔除不合法Unicode字符
            s.pop_back();
        }

        return gcnew System::String(s.c_str());
    }

    bool LocaleConverterManaged::IsUtf8CompatibleBytesManaged(array<byte>^ input)
    {
        if (input == nullptr || input->Length == 0)
            return true;

        std::vector<char> buffer(input->Length);
        for (int i = 0; i < input->Length; ++i)
            buffer[i] = input[i];

        return LocaleConverter::IsUtf8CompatibleBytes(buffer.data(), buffer.size());
    }
    
    
    System::String^ LocaleConverterManaged::GetSystemStringFromUtf8String(const std::string& input)
    {
        const std::wstring wide_string_opt = LocaleConverter::GetUtf16StringFromUtf8String(input);
        return gcnew System::String(wide_string_opt.c_str());
    }

}