// SPDX-License-Identifier: MIT

#include "pch.h"
#include "Platform/Windows/WindowsFileSystem.h"
#include "Platform/Windows/WindowsApiDiskFile.h"
#include "Platform/Common/VectorMemoryFile.h"
#include <utility>

namespace MusicPlayerLibrary {
	
	std::wstring WindowsApiFileSystem::GetFileExtension(const std::wstring& path)
	{
		return PathFindExtensionW(path.c_str());
	}

	bool WindowsApiFileSystem::FileExists(const std::wstring& file_path) const
	{
		const DWORD attributes = GetFileAttributesW(file_path.c_str());
		return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
	}

	std::unique_ptr<IFile> WindowsApiFileSystem::OpenReadFile(const std::wstring& file_path, bool share_deny_write, bool binary) const
	{
		UNREFERENCED_PARAMETER(binary);

		auto file = std::make_unique<WindowsApiDiskFile>();
		if (!file->Open(file_path, share_deny_write))
			return nullptr;

		return std::move(file);
	}

	std::unique_ptr<IFile> WindowsApiFileSystem::CreateTemporaryFile(bool binary) const
	{
		UNREFERENCED_PARAMETER(binary);

		auto file = std::make_unique<WindowsApiDiskFile>();
		if (!file->CreateTemporary())
			return nullptr;

		return std::move(file);
	}

	std::unique_ptr<IFile> WindowsApiFileSystem::CreateMemoryFile() const
	{
		return std::make_unique<VectorMemoryFile>();
	}

}
