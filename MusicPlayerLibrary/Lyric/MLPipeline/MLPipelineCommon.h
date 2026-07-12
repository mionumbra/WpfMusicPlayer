#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace MusicPlayerLibrary
{
    class IFile;
}

namespace MusicPlayerLibrary::MLPipeline
{
    constexpr std::array<char, 8> format_magic = { 'W', 'M', 'P', 'V', 'O', 'C', 'A', 'B' };
    constexpr std::uint32_t format_version = 1;
    constexpr std::size_t format_header_size =
        format_magic.size() + sizeof(std::uint32_t) * 2 + sizeof(std::uint64_t);
    constexpr std::size_t minimum_token_size = 1;
    constexpr std::size_t maximum_token_size = 3;
    constexpr std::uint64_t fnv_offset_basis = 14695981039346656037ull;
    constexpr std::uint64_t fnv_prime = 1099511628211ull;

    std::runtime_error format_error(
        const std::wstring& path,
        const std::string_view message);

    void read_exact(
        IFile& input,
        char* destination,
        const std::size_t size,
        const std::wstring& path);

    std::uint32_t read_u32_le(
        IFile& input,
        const std::wstring& path);

    std::uint64_t read_u64_le(
        IFile& input,
        const std::wstring& path);

    void fingerprint_bytes(
        std::uint64_t& fingerprint,
        const char* bytes,
        const std::size_t size);

    void fingerprint_u32_le(std::uint64_t& fingerprint, const std::uint32_t value);

    void fingerprint_u64_le(std::uint64_t& fingerprint, const std::uint64_t value);

    std::uint64_t begin_bundle_fingerprint(
        const std::uint64_t model_fingerprint,
        const std::uint32_t entry_count);

    void fingerprint_token(std::uint64_t& fingerprint, const std::string_view token);

    void fingerprint_file(
        std::uint64_t& fingerprint,
        const std::string_view label,
        const std::wstring& path);

    std::size_t parse_positive_size(
        const std::string_view text,
        const std::wstring& path,
        const std::string_view label);

    std::size_t read_ncnn_input_size(const std::wstring& path);
}
