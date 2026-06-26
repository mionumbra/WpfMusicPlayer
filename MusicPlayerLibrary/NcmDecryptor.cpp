// SPDX-License-Identifier: MIT

#include "pch.h"
#include "NcmDecryptor.h"
#include "LocaleConverter.h"

#include <openssl/evp.h>
#include <cpp-base64/base64.h>
#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include <algorithm>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>

static const uint8_t CORE_KEY[16] = {
    0x68, 0x7a, 0x48, 0x52, 0x41, 0x6d, 0x73, 0x6f,
    0x35, 0x6b, 0x49, 0x6e, 0x62, 0x61, 0x78, 0x57
};
static const uint8_t META_KEY[16] = {
    0x23, 0x31, 0x34, 0x6C, 0x6A, 0x6B, 0x5F, 0x21,
    0x5C, 0x5D, 0x26, 0x30, 0x55, 0x3C, 0x27, 0x28
};
static const uint8_t MAGIC_HEADER[8] = {
    0x43, 0x54, 0x45, 0x4e, 0x46, 0x44, 0x41, 0x4d
};
static constexpr ULONGLONG SeekFailure = static_cast<ULONGLONG>(-1);

static bool BytesHasPrefix(const uint8_t* buf, const uint8_t* prefix, size_t len)
{
    for (size_t i = 0; i < len; ++i)
    {
        if (buf[i] != prefix[i])
            return false;
    }
    return true;
}

static uint32_t ReadLEUint32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0])
        | static_cast<uint32_t>(p[1]) << 8
        | static_cast<uint32_t>(p[2]) << 16
        | static_cast<uint32_t>(p[3]) << 24;
}

static std::vector<uint8_t> Aes128EcbDecrypt(const std::vector<uint8_t>& cipher, const uint8_t key[16])
{
    std::vector<uint8_t> plain(cipher.size() + 16);
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
        throw std::runtime_error("EVP_CIPHER_CTX_new failed");
    int outlen1 = 0, outlen2 = 0;
    if (EVP_DecryptInit_ex(ctx, EVP_aes_128_ecb(), nullptr, key, nullptr) != 1)
    {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("EVP_DecryptInit_ex failed");
    }
    EVP_CIPHER_CTX_set_padding(ctx, 1);
    if (EVP_DecryptUpdate(ctx, plain.data(), &outlen1, cipher.data(), (int)cipher.size()) != 1)
    {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("EVP_DecryptUpdate failed");
    }
    if (EVP_DecryptFinal_ex(ctx, plain.data() + outlen1, &outlen2) != 1)
    {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("EVP_DecryptFinal_ex failed");
    }
    plain.resize(outlen1 + outlen2);
    EVP_CIPHER_CTX_free(ctx);
    return plain;
}

static std::vector<uint8_t> Base64DecodeVector(const std::string& s)
{
    std::string decoded = base64_decode(s);
    return { decoded.begin(), decoded.end() };
}

static std::wstring Utf8BytesToWideString(const char* input, size_t size)
{
    if (input == nullptr || size == 0)
        return {};
    if (size > static_cast<size_t>((std::numeric_limits<int>::max)()))
        return {};

    return MusicPlayerLibrary::LocaleConverterNative::GetUtf16StringFromUtf8String(std::string(input, size));
}

static std::wstring Utf8StringToWideString(const std::string& input)
{
    return Utf8BytesToWideString(input.data(), input.size());
}

namespace
{
    class NcmDecryptedAudioFile final : public MusicPlayerLibrary::IFile
    {
    public:
        NcmDecryptedAudioFile(
            std::unique_ptr<MusicPlayerLibrary::IFile> source_file,
            ULONGLONG audio_offset,
            ULONGLONG audio_length,
            std::vector<uint8_t> key_box)
            : source_file_(std::move(source_file)),
            audio_offset_(audio_offset),
            audio_length_(audio_length),
            key_box_(std::move(key_box))
        {
            if (!source_file_)
                throw std::runtime_error("invalid ncm source file");
            if (key_box_.size() != 256)
                throw std::runtime_error("invalid ncm key box");
        }

        ~NcmDecryptedAudioFile() override
        {
            Close();
        }

        UINT Read(void* buffer, UINT count) override
        {
            if (!source_file_ || buffer == nullptr || count == 0 || position_ >= audio_length_)
                return 0;

            const ULONGLONG bytes_to_read_64 =
                (std::min)(static_cast<ULONGLONG>(count), audio_length_ - position_);
            const UINT bytes_to_read = static_cast<UINT>(bytes_to_read_64);
            const ULONGLONG source_position = audio_offset_ + position_;
            if (source_position < audio_offset_
                || source_position > static_cast<ULONGLONG>((std::numeric_limits<LONGLONG>::max)()))
            {
                NATIVE_TRACE("err: ncm decrypted audio source position overflow\n");
                return 0;
            }

            const ULONGLONG actual_position =
                source_file_->Seek(static_cast<LONGLONG>(source_position), MusicPlayerLibrary::FileSeekOrigin::Begin);
            if (actual_position == SeekFailure || actual_position != source_position)
            {
                NATIVE_TRACE("err: seek ncm audio source failed\n");
                return 0;
            }

            auto* bytes = static_cast<uint8_t*>(buffer);
            const UINT bytes_read = source_file_->Read(bytes, bytes_to_read);
            for (UINT i = 0; i < bytes_read; ++i)
                bytes[i] ^= key_box_[(position_ + i) & 0xff];

            position_ += bytes_read;
            return bytes_read;
        }

        void Write(const void* buffer, UINT count) override
        {
            UNREFERENCED_PARAMETER(buffer);
            UNREFERENCED_PARAMETER(count);
            NATIVE_TRACE("err: ncm decrypted audio file is read-only\n");
        }

        ULONGLONG Seek(LONGLONG offset, MusicPlayerLibrary::FileSeekOrigin origin) override
        {
            ULONGLONG base_position;
            switch (origin)
            {
            case MusicPlayerLibrary::FileSeekOrigin::Begin:
                base_position = 0;
                break;
            case MusicPlayerLibrary::FileSeekOrigin::Current:
                base_position = position_;
                break;
            case MusicPlayerLibrary::FileSeekOrigin::End:
                base_position = audio_length_;
                break;
            default:
                base_position = 0;
                break;
            }

            ULONGLONG new_position;
            if (offset < 0)
            {
                const ULONGLONG distance = static_cast<ULONGLONG>(-(offset + 1)) + 1;
                if (distance > base_position)
                {
                    NATIVE_TRACE("err: ncm decrypted audio seek before begin\n");
                    return SeekFailure;
                }
                new_position = base_position - distance;
            }
            else
            {
                const ULONGLONG distance = static_cast<ULONGLONG>(offset);
                if (base_position > (std::numeric_limits<ULONGLONG>::max)() - distance)
                {
                    NATIVE_TRACE("err: ncm decrypted audio seek position overflow\n");
                    return SeekFailure;
                }
                new_position = base_position + distance;
            }

            position_ = new_position;
            return position_;
        }

        void SeekToBegin() override
        {
            position_ = 0;
        }

        ULONGLONG GetLength() const override
        {
            return audio_length_;
        }

        ULONGLONG GetPosition() const override
        {
            return position_;
        }

        void Close() override
        {
            if (source_file_)
            {
                source_file_->Close();
                source_file_.reset();
            }
            key_box_.clear();
            audio_offset_ = 0;
            audio_length_ = 0;
            position_ = 0;
        }

        bool GetReadBuffer(void** buffer_start, void** buffer_end) override
        {
            if (buffer_start)
                *buffer_start = nullptr;
            if (buffer_end)
                *buffer_end = nullptr;
            return false;
        }

    private:
        std::unique_ptr<IFile> source_file_;
        ULONGLONG audio_offset_ = 0;
        ULONGLONG audio_length_ = 0;
        ULONGLONG position_ = 0;
        std::vector<uint8_t> key_box_;
    };
}

void MusicPlayerLibrary::NcmDecryptor::EnsureSourceRange(ULONGLONG offset, ULONGLONG count, const char* message) const
{
    if (offset > m_fileLength || count > m_fileLength - offset)
        throw std::runtime_error(message);
}

void MusicPlayerLibrary::NcmDecryptor::SeekSource(ULONGLONG offset, const char* message)
{
    if (offset > static_cast<ULONGLONG>((std::numeric_limits<LONGLONG>::max)()))
        throw std::runtime_error(message);

    const ULONGLONG position = m_sourceFile.Seek(static_cast<LONGLONG>(offset), FileSeekOrigin::Begin);
    if (position == SeekFailure || position != offset)
        throw std::runtime_error(message);
}

void MusicPlayerLibrary::NcmDecryptor::ReadSourceExact(void* buffer, UINT count, const char* message)
{
    if (count == 0)
        return;
    if (buffer == nullptr)
        throw std::runtime_error(message);

    auto* bytes = static_cast<uint8_t*>(buffer);
    UINT total_read = 0;
    while (total_read < count)
    {
        const UINT bytes_read = m_sourceFile.Read(bytes + total_read, count - total_read);
        if (bytes_read == 0)
            throw std::runtime_error(message);
        total_read += bytes_read;
    }
}

uint32_t MusicPlayerLibrary::NcmDecryptor::ReadSourceUint32(const char* message)
{
    EnsureSourceRange(m_offset, 4, message);
    uint8_t bytes[4] = {};
    SeekSource(m_offset, message);
    ReadSourceExact(bytes, sizeof(bytes), message);
    m_offset += sizeof(bytes);
    return ReadLEUint32(bytes);
}

uint32_t MusicPlayerLibrary::NcmDecryptor::ReadSourceUint32At(ULONGLONG offset, const char* message)
{
    EnsureSourceRange(offset, 4, message);
    uint8_t bytes[4] = {};
    SeekSource(offset, message);
    ReadSourceExact(bytes, sizeof(bytes), message);
    return ReadLEUint32(bytes);
}

std::vector<uint8_t> MusicPlayerLibrary::NcmDecryptor::ReadSourceBytes(UINT count, const char* message)
{
    EnsureSourceRange(m_offset, count, message);
    std::vector<uint8_t> bytes(count);
    if (count > 0)
    {
        SeekSource(m_offset, message);
        ReadSourceExact(bytes.data(), count, message);
    }
    m_offset += count;
    return bytes;
}

static std::wstring JsonStringToWideString(const rapidjson::Value& value)
{
    if (!value.IsString())
        return {};
    return Utf8BytesToWideString(value.GetString(), value.GetStringLength());
}

static std::wstring JsonNumberToWideString(const rapidjson::Value& value)
{
    std::string number;
    if (value.IsInt64())
        number = std::to_string(value.GetInt64());
    else if (value.IsUint64())
        number = std::to_string(value.GetUint64());
    else if (value.IsNumber())
        number = std::to_string(value.GetDouble());
    else
        return {};

    return Utf8StringToWideString(number);
}

MusicPlayerLibrary::NcmDecryptor::NcmDecryptor(IFile& source_file, const std::wstring& filename)
    : m_sourceFile(source_file), m_fileLength(source_file.GetLength()), m_filename(filename) {
    EnsureSourceRange(0, 8, "ncm file too small");

    uint8_t header[8] = {};
    SeekSource(0, "failed to read ncm header");
    ReadSourceExact(header, sizeof(header), "failed to read ncm header");
    if (!BytesHasPrefix(header, MAGIC_HEADER, 8))
        throw std::runtime_error("此ncm文件已损坏");
    m_offset = 10;
}

MusicPlayerLibrary::NcmOpenResult MusicPlayerLibrary::NcmDecryptor::Open(
    std::unique_ptr<IFile> source_file,
    const std::wstring& filename)
{
    if (!source_file)
        throw std::runtime_error("invalid ncm source file");

    NcmDecryptor decryptor(*source_file, filename);
    auto keyBox = decryptor.GetKeyBox();
    decryptor.m_oriMeta = decryptor.GetMetaData();
    const ULONGLONG audioOffset = decryptor.GetAudioOffset();
    const ULONGLONG audioLength = decryptor.m_fileLength - audioOffset;

    NcmOpenResult openResult;
    openResult.metadata = decryptor.BuildDecryptResult();
    openResult.audio_file = std::make_unique<NcmDecryptedAudioFile>(
        std::move(source_file),
        audioOffset,
        audioLength,
        std::move(keyBox));
    return openResult;
}

MusicPlayerLibrary::DecryptResult MusicPlayerLibrary::NcmDecryptor::BuildDecryptResult()
{
    if (!m_oriMeta.format.empty())
        m_format = m_oriMeta.format;
    else
        m_format = L"mp3";
    if (m_format == L"mp3")
        m_mime = L"audio/mpeg";
    else if (m_format == L"flac")
        m_mime = L"audio/flac";
    else
        m_mime = L"application/octet-stream";
    DecryptResult res;
    res.ext = m_format;
    res.mime = m_mime;
    res.title = m_oriMeta.musicName;
    res.album = m_oriMeta.album;
    res.pictureUrl = m_oriMeta.albumPic;
    std::wstring artistJoined;
    for (auto& arr : m_oriMeta.artist)
    {
        if (!arr.empty())
        {
            if (!artistJoined.empty()) artistJoined += L"; ";
            artistJoined += arr[0];
        }
    }
    res.artist = artistJoined;
    return res;
}

std::vector<uint8_t> MusicPlayerLibrary::NcmDecryptor::GetKeyData()
{
    uint32_t keyLen = ReadSourceUint32("invalid ncm key length");
    std::vector<uint8_t> cipherText = ReadSourceBytes(keyLen, "invalid ncm key area");
    for (auto& byte : cipherText)
        byte ^= 0x64;
    auto plain = Aes128EcbDecrypt(cipherText, CORE_KEY);
    if (plain.size() <= 17)
        throw std::runtime_error("key data too short");
    std::vector result(plain.begin() + 17, plain.end());
    return result;
}

std::vector<uint8_t> MusicPlayerLibrary::NcmDecryptor::GetKeyBox()
{
    std::vector<uint8_t> keyData = GetKeyData();
    size_t keyLen = keyData.size();
    if (keyLen == 0)
        throw std::runtime_error("empty keyData");
    std::vector<uint8_t> box(256);
    for (int i = 0; i < 256; ++i)
        box[i] = static_cast<uint8_t>(i);
    uint8_t j = 0;
    for (int i = 0; i < 256; ++i)
    {
        j = static_cast<uint8_t>((box[i] + j + keyData[i % keyLen]) & 0xff);
        std::swap(box[i], box[j]);
    }
    std::vector<uint8_t> keyBox(256);
    for (int i = 0; i < 256; ++i)
    {
        int idx = (i + 1) & 0xff;
        uint8_t si = box[idx];
        uint8_t sj = box[(idx + si) & 0xff];
        keyBox[i] = box[(si + sj) & 0xff];
    }
    return keyBox;
}

MusicPlayerLibrary::NcmMusicMeta MusicPlayerLibrary::NcmDecryptor::GetMetaData()
{
    uint32_t metaDataLen = ReadSourceUint32("invalid meta length");
    if (metaDataLen == 0)
        return NcmMusicMeta{};
    std::vector<uint8_t> metaData = ReadSourceBytes(metaDataLen, "invalid meta area");
    for (auto& byte : metaData)
        byte ^= 0x63;
    if (metaData.size() <= 22)
        throw std::runtime_error("meta data too short");
    std::string base64Str(metaData.begin() + 22, metaData.end());
    auto decoded = Base64DecodeVector(base64Str);
    auto plain = Aes128EcbDecrypt(decoded, META_KEY);
    std::string text(plain.begin(), plain.end());
    size_t labelIndex = text.find(':');
    if (labelIndex == std::string::npos)
        throw std::runtime_error("invalid meta text");
    std::string label = text.substr(0, labelIndex);
    std::string jsonStr = text.substr(labelIndex + 1);
    using namespace rapidjson;
    Document doc;
    doc.Parse(jsonStr.c_str());
    if (doc.HasParseError())
    {
        throw std::runtime_error(std::string("meta json parse error: ") + GetParseError_En(doc.GetParseError()));
    }
    if (!doc.IsObject())
        throw std::runtime_error("meta json root is not object");
    NcmMusicMeta result;
    auto parseMusicMeta = [&](const Value& v, NcmMusicMeta& out)
        {
            if (!v.IsObject())
                return;

            auto readStringMember = [](const Value& source, const char* name, std::wstring& dest)
                {
                    const auto member = source.FindMember(name);
                    if (member != source.MemberEnd() && member->value.IsString())
                        dest = JsonStringToWideString(member->value);
                };
            auto appendJsonValue = [](const Value& item, std::vector<std::wstring>& dest)
                {
                    std::wstring value;
                    if (item.IsString())
                        value = JsonStringToWideString(item);
                    else if (item.IsNumber())
                        value = JsonNumberToWideString(item);

                    if (!value.empty())
                        dest.emplace_back(value);
                };

            readStringMember(v, "musicName", out.musicName);
            readStringMember(v, "album", out.album);
            readStringMember(v, "format", out.format);
            readStringMember(v, "albumPic", out.albumPic);

            const auto artistMember = v.FindMember("artist");
            if (artistMember != v.MemberEnd() && artistMember->value.IsArray())
            {
                const auto& arr = artistMember->value;
                for (SizeType i = 0; i < arr.Size(); ++i)
                {
                    std::vector<std::wstring> oneArtist;
                    const auto& item = arr[i];
                    if (item.IsArray())
                    {
                        for (SizeType j = 0; j < item.Size(); ++j)
                            appendJsonValue(item[j], oneArtist);
                    }
                    else
                        appendJsonValue(item, oneArtist);

                    if (!oneArtist.empty())
                        out.artist.push_back(std::move(oneArtist));
                }
            }
        };
    if (label == "dj")
    {
        const auto mainMusic = doc.FindMember("mainMusic");
        if (mainMusic == doc.MemberEnd() || !mainMusic->value.IsObject())
            throw std::runtime_error(
                "dj meta missing mainMusic");
        parseMusicMeta(mainMusic->value, result);
    }
    else { parseMusicMeta(doc, result); }
    if (!result.albumPic.empty())
    {
        // if (result.albumPic.rfind("http://", 0) == 0) result.albumPic.replace(0, 7, "https://");
        if (result.albumPic.rfind(L"http://", 0) == 0)
            result.albumPic = std::wstring(L"https://") + result.albumPic.substr(7);
        result.albumPic += L"?param=500y500";
    }
    std::wstring firstArtist;
    if (!result.artist.empty() && !result.artist[0].empty())
        firstArtist = result.artist[0][0];
    std::wstring jsonTrace = Utf8StringToWideString(jsonStr);

    NATIVE_TRACE(L"info: NcmDecrypt success!\n");
    NATIVE_TRACE(L"info: music name: %s\n", result.musicName.c_str());
    NATIVE_TRACE(L"info: album: %s\n", result.album.c_str());
    NATIVE_TRACE(L"info: artist: %s\n", firstArtist.c_str());
    NATIVE_TRACE(L"info: album pic: %s\n", result.albumPic.c_str());
    NATIVE_TRACE(L"info: format: %s\n", result.format.c_str());
    NATIVE_TRACE(L"info: meta data: %s\n", jsonTrace.c_str());
    return result;
}

ULONGLONG MusicPlayerLibrary::NcmDecryptor::GetAudioOffset()
{
    EnsureSourceRange(m_offset, 9, "invalid audio offset");
    uint32_t dataLen = ReadSourceUint32At(m_offset + 5, "invalid audio offset");
    constexpr ULONGLONG AudioHeaderSize = 13;
    if (AudioHeaderSize > m_fileLength - m_offset
        || static_cast<ULONGLONG>(dataLen) > m_fileLength - m_offset - AudioHeaderSize)
        throw std::runtime_error("audio offset out of range");
    m_offset += static_cast<ULONGLONG>(dataLen) + AudioHeaderSize;
    return m_offset;
}
