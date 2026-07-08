// SPDX-License-Identifier: MIT

#include "pch.h"
#include "LocaleConverter.h"

#include <iconv.h>
#include <uchardet/uchardet.h>
#include <msclr/marshal_cppstd.h>

#include <limits>

namespace
{
bool IsUtf8ContinuationByte(unsigned char value)
{
    return (value & 0xC0) == 0x80;
}

bool IsValidUtf8Bytes(const char* input, size_t size)
{
    const auto* bytes = reinterpret_cast<const unsigned char*>(input);
    size_t i = 0;
    while (i < size)
    {
        const unsigned char current = bytes[i];
        if (current <= 0x7F)
        {
            ++i;
            continue;
        }

        if (current >= 0xC2 && current <= 0xDF)
        {
            if (i + 1 >= size || !IsUtf8ContinuationByte(bytes[i + 1]))
                return false;
            i += 2;
            continue;
        }

        if (current == 0xE0)
        {
            if (i + 2 >= size
                || bytes[i + 1] < 0xA0
                || bytes[i + 1] > 0xBF
                || !IsUtf8ContinuationByte(bytes[i + 2]))
            {
                return false;
            }
            i += 3;
            continue;
        }

        if ((current >= 0xE1 && current <= 0xEC)
            || (current >= 0xEE && current <= 0xEF))
        {
            if (i + 2 >= size
                || !IsUtf8ContinuationByte(bytes[i + 1])
                || !IsUtf8ContinuationByte(bytes[i + 2]))
            {
                return false;
            }
            i += 3;
            continue;
        }

        if (current == 0xED)
        {
            if (i + 2 >= size
                || bytes[i + 1] < 0x80
                || bytes[i + 1] > 0x9F
                || !IsUtf8ContinuationByte(bytes[i + 2]))
            {
                return false;
            }
            i += 3;
            continue;
        }

        if (current == 0xF0)
        {
            if (i + 3 >= size
                || bytes[i + 1] < 0x90
                || bytes[i + 1] > 0xBF
                || !IsUtf8ContinuationByte(bytes[i + 2])
                || !IsUtf8ContinuationByte(bytes[i + 3]))
            {
                return false;
            }
            i += 4;
            continue;
        }

        if (current >= 0xF1 && current <= 0xF3)
        {
            if (i + 3 >= size
                || !IsUtf8ContinuationByte(bytes[i + 1])
                || !IsUtf8ContinuationByte(bytes[i + 2])
                || !IsUtf8ContinuationByte(bytes[i + 3]))
            {
                return false;
            }
            i += 4;
            continue;
        }

        if (current == 0xF4)
        {
            if (i + 3 >= size
                || bytes[i + 1] < 0x80
                || bytes[i + 1] > 0x8F
                || !IsUtf8ContinuationByte(bytes[i + 2])
                || !IsUtf8ContinuationByte(bytes[i + 3]))
            {
                return false;
            }
            i += 4;
            continue;
        }

        return false;
    }

    return true;
}

bool IsAsciiBytes(const char* input, size_t size)
{
    const auto* bytes = reinterpret_cast<const unsigned char*>(input);
    for (size_t i = 0; i < size; ++i)
    {
        if (bytes[i] > 0x7F)
            return false;
    }

    return true;
}

bool IsUtf8CompatibleCharset(const char* charset)
{
    if (!charset || strlen(charset) == 0)
        return true;

    return _stricmp(charset, "UTF-8") == 0
        || _stricmp(charset, "ASCII") == 0
        || _stricmp(charset, "US-ASCII") == 0
        || _stricmp(charset, "ANSI") == 0
        || _stricmp(charset, "ANSI_X3.4-1968") == 0;
}
}

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

bool MusicPlayerLibrary::LocaleConverterNative::IsUtf8CompatibleBytesNative(const char* input, size_t size)
{
    if (!input || size == 0) return true;

    auto uc_checker = uchardet_new();
    uchardet_handle_data(uc_checker, input, size);
    uchardet_data_end(uc_checker);
    const char* charset = uchardet_get_charset(uc_checker);
    NATIVE_TRACE("info: detected charset for UTF-8 validation = %s\n", charset);

    const bool charset_compatible = IsUtf8CompatibleCharset(charset);
    const bool bytes_compatible = IsValidUtf8Bytes(input, size);
    uchardet_delete(uc_checker);
    return bytes_compatible && (charset_compatible || IsAsciiBytes(input, size));
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

bool MusicPlayerLibrary::LocaleConverter::IsUtf8CompatibleBytes(array<byte>^ input)
{
    if (input == nullptr || input->Length == 0)
        return true;

    std::vector<char> buffer(input->Length);
    for (int i = 0; i < input->Length; ++i)
        buffer[i] = input[i];

    return LocaleConverterNative::IsUtf8CompatibleBytesNative(buffer.data(), buffer.size());
}
