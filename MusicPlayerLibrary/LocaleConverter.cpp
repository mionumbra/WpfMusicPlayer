#include "pch.h"
#include "LocaleConverter.h"

#include <iconv.h>
#include <uchardet/uchardet.h>
#include <msclr/marshal_cppstd.h>

std::string MusicPlayerLibrary::LocaleConverterNative::GetUtf8StringFromBytesNative(const char* input, size_t size)
{
    if (!input || size == 0) return {};

    auto uc_checker = uchardet_new();
    // 调用者负责约束input的有效性，因此不需要复制
    uchardet_handle_data(uc_checker, input, size);
    uchardet_data_end(uc_checker);
    const char* charset = uchardet_get_charset(uc_checker);
    ATLTRACE("info: detected charset = %s\n", charset);
    
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
    return StringUtils::FromUtf8Bytes(input.data(), input.size());
}

std::string MusicPlayerLibrary::LocaleConverterNative::GetUtf8StringFromUtf16String(const std::wstring& input)
{
    return StringUtils::ToUtf8(input);
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

