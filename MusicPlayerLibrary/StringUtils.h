#pragma once

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <cwchar>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace MusicPlayerLibrary::StringUtils
{
    inline int ToIndex(std::wstring_view::size_type position)
    {
        if (position == std::wstring_view::npos)
            return -1;
        return position > static_cast<size_t>((std::numeric_limits<int>::max)())
            ? (std::numeric_limits<int>::max)()
            : static_cast<int>(position);
    }

    inline int Find(std::wstring_view value, wchar_t ch, size_t start = 0)
    {
        return ToIndex(start > value.size() ? std::wstring_view::npos : value.find(ch, start));
    }

    inline int Find(std::wstring_view value, std::wstring_view token, size_t start = 0)
    {
        return ToIndex(start > value.size() ? std::wstring_view::npos : value.find(token, start));
    }

    inline std::wstring Left(std::wstring_view value, size_t count)
    {
        return std::wstring(value.substr(0, (std::min)(count, value.size())));
    }

    inline std::wstring Right(std::wstring_view value, size_t count)
    {
        count = (std::min)(count, value.size());
        return std::wstring(value.substr(value.size() - count));
    }

    inline std::wstring Mid(std::wstring_view value, size_t start, size_t count = std::wstring_view::npos)
    {
        if (start >= value.size())
            return {};
        return std::wstring(value.substr(start, count));
    }

    inline std::wstring Trim(std::wstring_view value)
    {
        size_t first = 0;
        while (first < value.size() && std::iswspace(value[first]) != 0)
            ++first;

        size_t last = value.size();
        while (last > first && std::iswspace(value[last - 1]) != 0)
            --last;

        return std::wstring(value.substr(first, last - first));
    }

    inline std::wstring Trim(std::wstring_view value, wchar_t ch)
    {
        size_t first = 0;
        while (first < value.size() && value[first] == ch)
            ++first;

        size_t last = value.size();
        while (last > first && value[last - 1] == ch)
            --last;

        return std::wstring(value.substr(first, last - first));
    }

    inline void ToLowerInPlace(std::wstring& value)
    {
        for (wchar_t& ch : value)
            ch = static_cast<wchar_t>(std::towlower(ch));
    }

    inline bool EqualsIgnoreCase(std::wstring_view left, std::wstring_view right)
    {
        if (left.size() != right.size())
            return false;

        for (size_t i = 0; i < left.size(); ++i)
        {
            if (std::towlower(left[i]) != std::towlower(right[i]))
                return false;
        }
        return true;
    }

    inline bool StartsWith(std::wstring_view value, std::wstring_view prefix)
    {
        return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
    }

    inline int ToIntOrZero(const std::wstring& value)
    {
        return static_cast<int>(std::wcstol(value.c_str(), nullptr, 10));
    }

    inline std::string FormatString(const char* format, ...)
    {
        va_list args;
        va_start(args, format);
        va_list args_copy;
        va_copy(args_copy, args);
        const int count = std::vsnprintf(nullptr, 0, format, args_copy);
        va_end(args_copy);

        if (count < 0)
        {
            va_end(args);
            return {};
        }

        std::vector<char> buffer(static_cast<size_t>(count) + 1);
        std::vsnprintf(buffer.data(), buffer.size(), format, args);
        va_end(args);
        return std::string(buffer.data(), static_cast<size_t>(count));
    }

    inline std::wstring FormatWide(const wchar_t* format, ...)
    {
        va_list args;
        va_start(args, format);
        va_list args_copy;
        va_copy(args_copy, args);
        const int count = _vscwprintf(format, args_copy);
        va_end(args_copy);

        if (count < 0)
        {
            va_end(args);
            return {};
        }

        std::vector<wchar_t> buffer(static_cast<size_t>(count) + 1);
        std::vswprintf(buffer.data(), buffer.size(), format, args);
        va_end(args);
        return std::wstring(buffer.data(), static_cast<size_t>(count));
    }

    inline std::wstring FromUtf8Bytes(const char* input, size_t size)
    {
        if (input == nullptr || size == 0)
            return {};
        if (size > static_cast<size_t>((std::numeric_limits<int>::max)()))
            return {};

        const int wide_len = MultiByteToWideChar(CP_UTF8, 0, input, static_cast<int>(size), nullptr, 0);
        if (wide_len <= 0)
            return {};

        std::wstring output(static_cast<size_t>(wide_len), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, input, static_cast<int>(size), output.data(), wide_len);
        return output;
    }

    inline std::string ToUtf8(std::wstring_view input)
    {
        if (input.empty())
            return {};
        if (input.size() > static_cast<size_t>((std::numeric_limits<int>::max)()))
            return {};

        const int size = static_cast<int>(input.size());
        const int utf8_len = WideCharToMultiByte(CP_UTF8, 0, input.data(), size, nullptr, 0, nullptr, nullptr);
        if (utf8_len <= 0)
            return {};

        std::string output(static_cast<size_t>(utf8_len), '\0');
        WideCharToMultiByte(CP_UTF8, 0, input.data(), size, output.data(), utf8_len, nullptr, nullptr);
        return output;
    }
}
