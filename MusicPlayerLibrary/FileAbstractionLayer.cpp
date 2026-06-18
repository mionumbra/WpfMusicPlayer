#include "pch.h"
#include "FileAbstractionLayer.h"
#include <limits>
#include <utility>

namespace MusicPlayerLibrary {

	namespace
	{
		constexpr ULONGLONG SeekFailure = static_cast<ULONGLONG>(-1);

		DWORD ToWindowsSeekOrigin(FileSeekOrigin origin)
		{
			switch (origin)
			{
			case FileSeekOrigin::Begin:
				return FILE_BEGIN;
			case FileSeekOrigin::Current:
				return FILE_CURRENT;
			case FileSeekOrigin::End:
				return FILE_END;
			default:
				return FILE_BEGIN;
			}
		}
	}

	class WindowsApiDiskFile final : public IFile
	{
	public:
		~WindowsApiDiskFile() override
		{
			Close();
		}

		bool Open(const std::wstring& file_path, bool share_deny_write)
		{
			Close();

			DWORD share_mode = FILE_SHARE_READ;
			if (!share_deny_write)
				share_mode |= FILE_SHARE_WRITE;

			file_ = CreateFileW(
				file_path.c_str(),
				GENERIC_READ,
				share_mode,
				nullptr,
				OPEN_EXISTING,
				FILE_ATTRIBUTE_NORMAL,
				nullptr);
			if (file_ == INVALID_HANDLE_VALUE)
			{
				ATLTRACE(L"err: CreateFileW failed:%ls, gle=%lu\n", file_path.c_str(), ::GetLastError());
				return false;
			}

			return true;
		}

		UINT Read(void* buffer, UINT count) override
		{
			if (file_ == INVALID_HANDLE_VALUE || buffer == nullptr || count == 0)
				return 0;

			DWORD bytes_read = 0;
			if (!ReadFile(file_, buffer, count, &bytes_read, nullptr))
			{
				ATLTRACE("err: ReadFile failed, gle=%lu\n", ::GetLastError());
				return 0;
			}

			return static_cast<UINT>(bytes_read);
		}

		void Write(const void* buffer, UINT count) override
		{
			if (file_ == INVALID_HANDLE_VALUE || buffer == nullptr || count == 0)
				return;

			DWORD bytes_written = 0;
			if (!WriteFile(file_, buffer, count, &bytes_written, nullptr)
				|| bytes_written != static_cast<DWORD>(count))
			{
				ATLTRACE("err: WriteFile failed, gle=%lu\n", ::GetLastError());
			}
		}

		ULONGLONG Seek(LONGLONG offset, FileSeekOrigin origin) override
		{
			if (file_ == INVALID_HANDLE_VALUE)
				return SeekFailure;

			LARGE_INTEGER distance = {
				.QuadPart = offset
			};
			LARGE_INTEGER new_position = {};
			if (!SetFilePointerEx(file_, distance, &new_position, ToWindowsSeekOrigin(origin)))
			{
				ATLTRACE("err: SetFilePointerEx failed, gle=%lu\n", ::GetLastError());
				return SeekFailure;
			}

			return static_cast<ULONGLONG>(new_position.QuadPart);
		}

		void SeekToBegin() override
		{
			Seek(0, FileSeekOrigin::Begin);
		}

		ULONGLONG GetLength() const override
		{
			if (file_ == INVALID_HANDLE_VALUE)
				return 0;

			LARGE_INTEGER file_size = {};
			if (!GetFileSizeEx(file_, &file_size))
			{
				ATLTRACE("err: GetFileSizeEx failed, gle=%lu\n", ::GetLastError());
				return 0;
			}

			return static_cast<ULONGLONG>(file_size.QuadPart);
		}

		ULONGLONG GetPosition() const override
		{
			if (file_ == INVALID_HANDLE_VALUE)
				return 0;

			LARGE_INTEGER distance = {};
			LARGE_INTEGER position = {};
			if (!SetFilePointerEx(file_, distance, &position, FILE_CURRENT))
			{
				ATLTRACE("err: SetFilePointerEx failed, gle=%lu\n", ::GetLastError());
				return 0;
			}

			return static_cast<ULONGLONG>(position.QuadPart);
		}

		void Close() override
		{
			if (file_ != INVALID_HANDLE_VALUE)
			{
				CloseHandle(file_);
				file_ = INVALID_HANDLE_VALUE;
			}
		}

		bool GetReadBuffer(void** buffer_start, void** buffer_end) override
		{
			UNREFERENCED_PARAMETER(buffer_start);
			UNREFERENCED_PARAMETER(buffer_end);
			return false;
		}

	private:
		HANDLE file_ = INVALID_HANDLE_VALUE;
	};

	class VectorMemoryFile final : public IFile
	{
	public:
		~VectorMemoryFile() override
		{
			Close();
		}

		UINT Read(void* buffer, UINT count) override
		{
			if (buffer == nullptr || count == 0 || position_ >= data_.size())
				return 0;

			const size_t read_position = position_;
			const size_t bytes_to_read = (std::min)(static_cast<size_t>(count), data_.size() - read_position);
			std::memcpy(buffer, data_.data() + read_position, bytes_to_read);
			position_ += bytes_to_read;
			return static_cast<UINT>(bytes_to_read);
		}

		void Write(const void* buffer, UINT count) override
		{
			if (buffer == nullptr || count == 0)
				return;

			if (position_ > data_.max_size())
			{
				ATLTRACE("err: memory file write position is too large\n");
				return;
			}

			const size_t write_position = position_;
			if (static_cast<size_t>(count) > data_.max_size() - write_position)
			{
				ATLTRACE("err: memory file write size is too large\n");
				return;
			}

			const size_t write_end = write_position + static_cast<size_t>(count);
			if (write_end > data_.size())
				data_.resize(write_end);

			std::memcpy(data_.data() + write_position, buffer, count);
			position_ = write_end;
		}

		ULONGLONG Seek(LONGLONG offset, FileSeekOrigin origin) override
		{
			ULONGLONG base_position;
			switch (origin)
			{
			case FileSeekOrigin::Begin:
				base_position = 0;
				break;
			case FileSeekOrigin::Current:
				base_position = position_;
				break;
			case FileSeekOrigin::End:
				base_position = data_.size();
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
					ATLTRACE("err: memory file seek before begin\n");
					return SeekFailure;
				}
				new_position = base_position - distance;
			}
			else
			{
				const ULONGLONG distance = static_cast<ULONGLONG>(offset);
				if (base_position > (std::numeric_limits<ULONGLONG>::max)() - distance)
				{
					ATLTRACE("err: memory file seek position overflow\n");
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
			return data_.size();
		}

		ULONGLONG GetPosition() const override
		{
			return position_;
		}

		void Close() override
		{
			std::vector<BYTE>().swap(data_);
			position_ = 0;
		}

		bool GetReadBuffer(void** buffer_start, void** buffer_end) override
		{
			if (buffer_start)
				*buffer_start = nullptr;
			if (buffer_end)
				*buffer_end = nullptr;
			if (buffer_start == nullptr || buffer_end == nullptr || data_.empty())
				return false;

			*buffer_start = data_.data();
			*buffer_end = data_.data() + data_.size();
			return true;
		}

	private:
		std::vector<BYTE> data_;
		ULONGLONG position_ = 0;
	};

	bool WindowsApiFileSystem::FileExists(const std::wstring& file_path) const
	{
		const DWORD attributes = ::GetFileAttributesW(file_path.c_str());
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

	std::unique_ptr<IFile> WindowsApiFileSystem::CreateMemoryFile() const
	{
		return std::make_unique<VectorMemoryFile>();
	}

	static IFileSystem* custom_file_system = nullptr;
	static FileSystemImplementation default_file_system_implementation = FileSystemImplementation::WindowsApi;

	IFileSystem& GetBuiltInFileSystem(FileSystemImplementation implementation)
	{
		static WindowsApiFileSystem windows_api_file_system;

		switch (implementation)
		{
		case FileSystemImplementation::WindowsApi:
		default:
			return windows_api_file_system;
		}
	}

	FileSystemImplementation GetDefaultFileSystemImplementation()
	{
		return default_file_system_implementation;
	}

	void SetDefaultFileSystemImplementation(FileSystemImplementation implementation)
	{
		default_file_system_implementation = implementation;
		custom_file_system = nullptr;
	}

	IFileSystem& GetDefaultFileSystem()
	{
		if (custom_file_system)
			return *custom_file_system;
		return GetBuiltInFileSystem(default_file_system_implementation);
	}

	void SetDefaultFileSystem(IFileSystem* file_system)
	{
		custom_file_system = file_system;
	}

}
