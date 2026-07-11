// SPDX-License-Identifier: MIT

#include "pch.h"
#include "Core/LocaleConverter.h"

#include <iconv.h>
#include <uchardet/uchardet.h>

#include <cctype>
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

bool EqualsAsciiIgnoreCase(const char* left, const char* right)
{
    if (!left || !right)
        return left == right;

    while (*left != '\0' && *right != '\0')
    {
        const auto left_char = static_cast<unsigned char>(*left);
        const auto right_char = static_cast<unsigned char>(*right);
        if (std::tolower(left_char) != std::tolower(right_char))
            return false;
        ++left;
        ++right;
    }
    return *left == *right;
}

bool IsUtf8CompatibleCharset(const char* charset)
{
    if (!charset || strlen(charset) == 0)
        return true;

    return EqualsAsciiIgnoreCase(charset, "UTF-8")
        || EqualsAsciiIgnoreCase(charset, "ASCII")
        || EqualsAsciiIgnoreCase(charset, "US-ASCII")
        || EqualsAsciiIgnoreCase(charset, "ANSI")
        || EqualsAsciiIgnoreCase(charset, "ANSI_X3.4-1968");
}
}

std::string MusicPlayerLibrary::LocaleConverter::GetUtf8StringFromBytes(const char* input, size_t size)
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
    if (!charset || strlen(charset) == 0 || EqualsAsciiIgnoreCase(charset, "UTF-8")) {
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

bool MusicPlayerLibrary::LocaleConverter::IsUtf8CompatibleBytes(const char* input, size_t size)
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

std::wstring MusicPlayerLibrary::LocaleConverter::GetUtf16StringFromUtf8String(const std::string& input)
{
    if (input.empty())
        return {};
    if (input.size() > static_cast<size_t>((std::numeric_limits<int>::max)()))
        return {};

    iconv_t iconver = iconv_open("UTF-16LE", "UTF-8");
    
    if (iconver == reinterpret_cast<iconv_t>(-1)) {
        return {};
    }

    size_t insize = input.size();
    size_t outcapacity = insize * sizeof(wchar_t) * 2;
    size_t outleft = outcapacity;

    std::wstring outbuf(outcapacity / sizeof(wchar_t), L'\0');

    char* pIn  = const_cast<char*>(input.data());
    char* pOut = reinterpret_cast<char*>(outbuf.data());

    size_t res = iconv(iconver, &pIn, &insize, &pOut, &outleft);
    iconv_close(iconver);

    if (res == static_cast<size_t>(-1))
        return {};

    size_t actualBytes = outcapacity - outleft;
    outbuf.resize(actualBytes / sizeof(wchar_t));
    return outbuf;
}

std::string MusicPlayerLibrary::LocaleConverter::GetUtf8StringFromUtf16String(const std::wstring& input)
{
    if (input.empty())
        return {};
    if (input.size() > static_cast<size_t>((std::numeric_limits<int>::max)()))
        return {};

    iconv_t iconver = iconv_open("UTF-8", "UTF-16LE");
    
    if (iconver == reinterpret_cast<iconv_t>(-1)) {
        return {};
    }

    size_t insize = input.size() * sizeof(wchar_t);   // must be num of bytes
    size_t outcapacity = insize * 2;   // UTF-8 encoding may expand to 4*in_size
    size_t outleft = outcapacity;
    
    std::string outbuf(outcapacity, '\0');
    char* pOriginalStart = outbuf.data();
    char* pIn = const_cast<char*>(reinterpret_cast<const char*>(input.c_str()));
    char* pOut = pOriginalStart;

    size_t res = iconv(iconver, &pIn, &insize, &pOut, &outleft);
    
    iconv_close(iconver); 

    if (res == static_cast<size_t>(-1)) {
        return {};
    }

    // Shrink actualSize to fit
    size_t actualSize = outcapacity - outleft;
    outbuf.resize(actualSize); 

    return outbuf;
}
