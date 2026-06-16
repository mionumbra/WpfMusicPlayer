#pragma once

#include <memory>
#include <string>

namespace MusicPlayerLibrary {

	enum class FileSeekOrigin
	{
		Begin,
		Current,
		End
	};

	class IFile
	{
	public:
		virtual ~IFile() = default;

		virtual UINT Read(void* buffer, UINT count) = 0;
		virtual void Write(const void* buffer, UINT count) = 0;
		virtual ULONGLONG Seek(LONGLONG offset, FileSeekOrigin origin) = 0;
		virtual void SeekToBegin() = 0;
		virtual ULONGLONG GetLength() const = 0;
		virtual ULONGLONG GetPosition() const = 0;
		virtual void Close() = 0;
		virtual bool GetReadBuffer(void** buffer_start, void** buffer_end) = 0;

		IFile() = default;
		IFile(const IFile&) = delete;
		IFile& operator=(const IFile&) = delete;
	};

	class IFileSystem
	{
	public:
		virtual ~IFileSystem() = default;

		virtual bool FileExists(const std::wstring& file_path) const = 0;
		virtual std::unique_ptr<IFile> OpenReadFile(const std::wstring& file_path, bool share_deny_write, bool binary) const = 0;
		virtual std::unique_ptr<IFile> CreateMemoryFile() const = 0;
	};

	enum class FileSystemImplementation
	{
		WindowsApi
	};

	class WindowsApiFileSystem final : public IFileSystem
	{
	public:
		bool FileExists(const std::wstring& file_path) const override;
		std::unique_ptr<IFile> OpenReadFile(const std::wstring& file_path, bool share_deny_write, bool binary) const override;
		std::unique_ptr<IFile> CreateMemoryFile() const override;
	};

	IFileSystem& GetBuiltInFileSystem(FileSystemImplementation implementation);
	FileSystemImplementation GetDefaultFileSystemImplementation();
	void SetDefaultFileSystemImplementation(FileSystemImplementation implementation);
	IFileSystem& GetDefaultFileSystem();
	void SetDefaultFileSystem(IFileSystem* file_system);

}
