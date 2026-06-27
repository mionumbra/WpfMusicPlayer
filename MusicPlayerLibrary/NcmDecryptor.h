// SPDX-License-Identifier: MIT

#pragma once
#include "pch.h"
#include "FileAbstractionLayer.h"

namespace MusicPlayerLibrary
{
    public struct NcmMusicMeta
    {
        std::wstring musicName;
        std::vector<std::vector<std::wstring>> artist;
        std::wstring format;
        std::wstring album;
        std::wstring albumPic;
    };

    public struct DecryptResult
    {
        std::wstring title;
        std::wstring artist;
        std::wstring album;
        std::wstring ext;
        std::wstring pictureUrl;
        std::wstring mime;
    };

    public struct NcmOpenResult
    {
        DecryptResult metadata;
        std::unique_ptr<IFile> audio_file;
    };

    public class NcmDecryptor
    {
    public:
        static NcmOpenResult Open(std::unique_ptr<IFile> source_file, const std::wstring& filename);

    private:
        NcmDecryptor(IFile& source_file, const std::wstring& filename);

        IFile& m_sourceFile;
        ULONGLONG m_fileLength = 0;
        ULONGLONG m_offset = 0;
        std::wstring m_filename;

        NcmMusicMeta m_oriMeta;
        std::wstring m_format;
        std::wstring m_mime;

        void EnsureSourceRange(uint64_t offset, uint64_t count, const char* message) const;
        void SeekSource(uint64_t offset, const char* message);
        void ReadSourceExact(void* buffer, uint32_t count, const char* message);
        uint32_t ReadSourceUint32(const char* message);
        uint32_t ReadSourceUint32At(uint64_t offset, const char* message);
        std::vector<uint8_t> ReadSourceBytes(uint32_t count, const char* message);
        std::vector<uint8_t> GetKeyData();
        std::vector<uint8_t> GetKeyBox();
        NcmMusicMeta GetMetaData();
        ULONGLONG GetAudioOffset();
        DecryptResult BuildDecryptResult();
    };
}