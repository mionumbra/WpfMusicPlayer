// SPDX-License-Identifier: MIT

#include "pch.h"
#include "LocaleConverter.h"

#include <iconv.h>
#include <uchardet/uchardet.h>
#include <msclr/marshal_cppstd.h>

#include <limits>

std::string MusicPlayerLibrary::LocaleConverterNative::GetUtf8StringFromBytesNative(const char* input, size_t size)
{
    if (!input || size == 0) return {};

    auto uc_checker = uchardet_new();
    // 调用者负责约束input的有效性，因此不需要复制
    uchardet_handle_data(uc_checker, input, size);
    uchardet_data_end(uc_checker);
    const char* charset = uchardet_get_charset(uc_checker);
    NATIVE_TRACE("info: detected charset = %s\n", charset);
    
    // if is utf-8 or ansi...
    // ansi is a subset of UTF-8
    // conversion guard
    if (!charset || strlen(charset) == 0 || _stricmp(charset, "UTF-8") == 0) {
        uchardet_delete(uc_checker);
        return std::string(input, size);
    }

    iconv_t iconver = iconv_open("UTF-8", charset);
    // fix: release uc_checker
    uchardet_delete(uc_checker);

    if (iconver == reinterpret_cast<iconv_t>(-1)) {
        return std::string(input, size);
    }

    size_t insize = size;
    // UTF-8 encoding may expand to 4*in_size
    size_t outcapacity = size * 4; 
    size_t outleft = outcapacity;
    
    std::string outbuf(outcapacity, '\0');
    char* pOriginalStart = outbuf.data();
    char* pIn = const_cast<char*>(input);
    char* pOut = pOriginalStart;

    size_t res = iconv(iconver, &pIn, &insize, &pOut, &outleft);
    
    // fix: release iconver
    iconv_close(iconver); 

    if (res == static_cast<size_t>(-1)) {
        return std::string(input, size);
    }

    // Shrink actualSize to fit
    size_t actualSize = outcapacity - outleft;
    outbuf.resize(actualSize); 

    return outbuf;
}

std::wstring MusicPlayerLibrary::LocaleConverterNative::GetUtf16StringFromUtf8String(const std::string& input)
{
    if (input.empty())
        return {};
    if (input.size() > static_cast<size_t>((std::numeric_limits<int>::max)()))
        return {};

    const int input_size = static_cast<int>(input.size());
    const int wide_length = MultiByteToWideChar(CP_UTF8, 0, input.data(), input_size, nullptr, 0);
    if (wide_length <= 0)
        return {};

    std::wstring output(static_cast<size_t>(wide_length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, input.data(), input_size, output.data(), wide_length);
    return output;
}

std::string MusicPlayerLibrary::LocaleConverterNative::GetUtf8StringFromUtf16String(const std::wstring& input)
{
    if (input.empty())
        return {};
    if (input.size() > static_cast<size_t>((std::numeric_limits<int>::max)()))
        return {};

    const int input_size = static_cast<int>(input.size());
    const int utf8_length = WideCharToMultiByte(CP_UTF8, 0, input.data(), input_size, nullptr, 0, nullptr, nullptr);
    if (utf8_length <= 0)
        return {};

    std::string output(static_cast<size_t>(utf8_length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, input.data(), input_size, output.data(), utf8_length, nullptr, nullptr);
    return output;
}

System::String^ MusicPlayerLibrary::LocaleConverterNative::GetSystemStringFromUtf8String(const std::string& input)
{
    const std::wstring wide_string_opt = GetUtf16StringFromUtf8String(input);
    return gcnew System::String(wide_string_opt.c_str());
}

System::String^ MusicPlayerLibrary::LocaleConverter::GetSystemStringFromBytes(array<byte>^ input)
{
    if (input == nullptr || input->Length == 0)
        return gcnew System::String(L"");

    std::vector<char> buffer(input->Length);
    for (int i = 0; i < input->Length; ++i)
        buffer[i] = input[i];

    std::string utf8 = LocaleConverterNative::GetUtf8StringFromBytesNative(buffer.data(), buffer.size());
    std::wstring s = LocaleConverterNative::GetUtf16StringFromUtf8String(utf8);

    while (!s.empty())
    {
        WCHAR ch = s.back();
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

