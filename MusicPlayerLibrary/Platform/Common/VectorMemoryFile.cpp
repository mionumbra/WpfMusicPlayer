// SPDX-License-Identifier: MIT

#include "pch.h"
#include "Platform/Common/VectorMemoryFile.h"
#include <limits>
#include <utility>

namespace MusicPlayerLibrary
{

    VectorMemoryFile::~VectorMemoryFile()
    {
        Close();
    }

    uint32_t VectorMemoryFile::Read(void* buffer, uint32_t count)
    {
        if (buffer == nullptr || count == 0 || position_ >= data_.size())
            return 0;

        const size_t read_position = position_;
        const size_t bytes_to_read = (std::min)(static_cast<size_t>(count), data_.size() - read_position);
        std::memcpy(buffer, data_.data() + read_position, bytes_to_read);
        position_ += bytes_to_read;
        return static_cast<uint32_t>(bytes_to_read);
    }

    void VectorMemoryFile::Write(const void* buffer, uint32_t count)
    {
        if (buffer == nullptr || count == 0)
            return;

        if (position_ > data_.max_size())
        {
            NATIVE_TRACE("err: memory file write position is too large\n");
            return;
        }

        const size_t write_position = position_;
        if (static_cast<size_t>(count) > data_.max_size() - write_position)
        {
            NATIVE_TRACE("err: memory file write size is too large\n");
            return;
        }

        const size_t write_end = write_position + static_cast<size_t>(count);
        if (write_end > data_.size())
            data_.resize(write_end);

        std::memcpy(data_.data() + write_position, buffer, count);
        position_ = write_end;
    }

    uint64_t VectorMemoryFile::Seek(int64_t offset, FileSeekOrigin origin)
    {
        uint64_t base_position;
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

        uint64_t new_position;
        if (offset < 0)
        {
            const uint64_t distance = static_cast<uint64_t>(-(offset + 1)) + 1;
            if (distance > base_position)
            {
                NATIVE_TRACE("err: memory file seek before begin\n");
                return SeekFailure;
            }
            new_position = base_position - distance;
        }
        else
        {
            const uint64_t distance = static_cast<uint64_t>(offset);
            if (base_position > (std::numeric_limits<uint64_t>::max)() - distance)
            {
                NATIVE_TRACE("err: memory file seek position overflow\n");
                return SeekFailure;
            }
            new_position = base_position + distance;
        }

        position_ = new_position;
        return position_;
    }

    void VectorMemoryFile::SeekToBegin()
    {
        position_ = 0;
    }

    uint64_t VectorMemoryFile::GetLength() const
    {
        return data_.size();
    }

    uint64_t VectorMemoryFile::GetPosition() const
    {
        return position_;
    }

    void VectorMemoryFile::Close()
    {
        decltype(data_){}.swap(data_);
        position_ = 0;
    }

    bool VectorMemoryFile::GetReadBuffer(void** buffer_start, void** buffer_end)
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
}
