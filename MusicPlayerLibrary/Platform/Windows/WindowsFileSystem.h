// SPDX-License-Identifier: MIT

#pragma once
#include "Core/FileAbstractionLayer.h"

namespace MusicPlayerLibrary
{
    class WindowsApiFileSystem final : public IFileSystem
    {
    public:
        std::wstring GetFileExtension(const std::wstring& path);
        bool FileExists(const std::wstring& file_path) const override;
        std::unique_ptr<IFile> OpenReadFile(const std::wstring& file_path, bool share_deny_write, bool binary) const override;
        std::unique_ptr<IFile> CreateTemporaryFile(bool binary) const override;
        std::unique_ptr<IFile> CreateMemoryFile() const override;
    };
}
