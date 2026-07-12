#include "pch.h"
#include "Lyric/MLPipeline/MLPipelineCommon.h"
#include "Core/FileAbstractionLayer.h"
#include "Core/LocaleConverter.h"

#include <charconv>
#include <limits>
#include <sstream>
#include <vector>

namespace
{
    std::string display_path(const std::wstring& path)
    {
        const auto utf8_path = MusicPlayerLibrary::LocaleConverter::GetUtf8StringFromUtf16String(path);
        return utf8_path.empty() && !path.empty() ? "<unrepresentable path>" : utf8_path;
    }

    std::unique_ptr<MusicPlayerLibrary::IFile> open_read_file(
        const std::wstring& path,
        const std::string_view description)
    {
        auto file = MusicPlayerLibrary::GetDefaultFileSystem().OpenReadFile(path, true, true);
        if (!file)
        {
            throw std::runtime_error(
                "failed to open " + std::string(description) + ": " + display_path(path));
        }
        return file;
    }

    std::string read_text_file(const std::wstring& path, const std::string_view description)
    {
        auto file = open_read_file(path, description);
        const auto length = file->GetLength();
        if (length > (std::numeric_limits<std::size_t>::max)())
            throw std::runtime_error(std::string(description) + " is too large: " + display_path(path));

        std::string contents(length, '\0');
        MusicPlayerLibrary::MLPipeline::read_exact(*file, contents.data(), contents.size(), path);
        return contents;
    }
}

std::runtime_error MusicPlayerLibrary::MLPipeline::format_error(const std::wstring& path,
    const std::string_view message)
{
    return std::runtime_error(
        "invalid vocabulary file '" + display_path(path) + "': " + std::string(message));
}

void MusicPlayerLibrary::MLPipeline::read_exact(IFile& input, char* destination, const std::size_t size,
    const std::wstring& path)
{
    std::size_t total_read = 0;
    while (total_read < size)
    {
        const auto remaining = size - total_read;
        const auto request_size = static_cast<std::uint32_t>(std::min(
            remaining,
            static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)())));
        const auto bytes_read = input.Read(destination + total_read, request_size);
        if (bytes_read == 0)
            throw format_error(path, "unexpected end of file");
        total_read += bytes_read;
    }
}

std::uint32_t MusicPlayerLibrary::MLPipeline::read_u32_le(IFile& input, const std::wstring& path)
{
    std::array<unsigned char, 4> bytes{};
    read_exact(
        input,
        reinterpret_cast<char*>(bytes.data()),
        bytes.size(),
        path);
    return static_cast<std::uint32_t>(bytes[0]) |
        (static_cast<std::uint32_t>(bytes[1]) << 8) |
        (static_cast<std::uint32_t>(bytes[2]) << 16) |
        (static_cast<std::uint32_t>(bytes[3]) << 24);
}

std::uint64_t MusicPlayerLibrary::MLPipeline::read_u64_le(IFile& input, const std::wstring& path)
{
    std::array<unsigned char, 8> bytes{};
    read_exact(
        input,
        reinterpret_cast<char*>(bytes.data()),
        bytes.size(),
        path);

    std::uint64_t value{};
    for (std::size_t i = 0; i < bytes.size(); ++i)
        value |= static_cast<std::uint64_t>(bytes[i]) << (i * 8);
    return value;
}

void MusicPlayerLibrary::MLPipeline::fingerprint_bytes(std::uint64_t& fingerprint, const char* bytes,
    const std::size_t size)
{
    for (std::size_t i = 0; i < size; ++i)
    {
        fingerprint ^= static_cast<unsigned char>(bytes[i]);
        fingerprint *= fnv_prime;
    }
}

void MusicPlayerLibrary::MLPipeline::fingerprint_u32_le(std::uint64_t& fingerprint, const std::uint32_t value)
{
    std::array<char, 4> bytes{};
    for (std::size_t i = 0; i < bytes.size(); ++i)
        bytes[i] = static_cast<char>(value >> (i * 8) & 0xff);
    fingerprint_bytes(fingerprint, bytes.data(), bytes.size());
}

void MusicPlayerLibrary::MLPipeline::fingerprint_u64_le(std::uint64_t& fingerprint, const std::uint64_t value)
{
    std::array<char, 8> bytes{};
    for (std::size_t i = 0; i < bytes.size(); ++i)
        bytes[i] = static_cast<char>(value >> (i * 8) & 0xff);
    fingerprint_bytes(fingerprint, bytes.data(), bytes.size());
}

std::uint64_t MusicPlayerLibrary::MLPipeline::begin_bundle_fingerprint(const std::uint64_t model_fingerprint,
    const std::uint32_t entry_count)
{
    std::uint64_t fingerprint = fnv_offset_basis;
    constexpr std::string_view domain = "WMP-VOCAB-BUNDLE-V1";
    fingerprint_bytes(fingerprint, domain.data(), domain.size());
    fingerprint_u64_le(fingerprint, model_fingerprint);
    fingerprint_u32_le(fingerprint, entry_count);
    return fingerprint;
}

void MusicPlayerLibrary::MLPipeline::fingerprint_token(std::uint64_t& fingerprint, const std::string_view token)
{
    const char size = static_cast<char>(token.size());
    fingerprint_bytes(fingerprint, &size, 1);
    fingerprint_bytes(fingerprint, token.data(), token.size());
}

void MusicPlayerLibrary::MLPipeline::fingerprint_file(std::uint64_t& fingerprint, const std::string_view label,
    const std::wstring& path)
{
    fingerprint_u64_le(fingerprint, static_cast<std::uint64_t>(label.size()));
    fingerprint_bytes(fingerprint, label.data(), label.size());

    auto input = open_read_file(path, "NCNN model file");
    const auto file_size = input->GetLength();
    fingerprint_u64_le(fingerprint, file_size);

    std::array<char, 64 * 1024> buffer{};
    std::uint64_t bytes_read{};
    while (bytes_read < file_size)
    {
        const auto request_size = static_cast<std::uint32_t>(std::min(
            buffer.size(), file_size - bytes_read));
        const auto count = input->Read(buffer.data(), request_size);
        if (count == 0)
            throw std::runtime_error("failed to fingerprint NCNN model file: " + display_path(path));
        fingerprint_bytes(fingerprint, buffer.data(), count);
        bytes_read += count;
    }
}

std::size_t MusicPlayerLibrary::MLPipeline::parse_positive_size(const std::string_view text,
    const std::wstring& path, const std::string_view label)
{
    std::uint64_t value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value == 0 ||
        value > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
    {
        throw std::runtime_error(
            "invalid " + std::string(label) + " in NCNN param file: " + display_path(path));
    }
    return value;
}

std::size_t MusicPlayerLibrary::MLPipeline::read_ncnn_input_size(const std::wstring& path)
{
    const auto contents = read_text_file(path, "NCNN param file");
    std::istringstream input(contents);

    std::string magic;
    if (!(input >> magic) || magic != "7767517")
        throw std::runtime_error("invalid NCNN param magic: " + display_path(path));

    std::size_t layer_count{};
    std::size_t blob_count{};
    if (!(input >> layer_count >> blob_count) || layer_count == 0 || blob_count == 0)
        throw std::runtime_error("invalid NCNN layer/blob count: " + display_path(path));

    std::string line;
    std::getline(input, line);
    for (std::size_t layer_index = 0; layer_index < layer_count; ++layer_index)
    {
        if (!std::getline(input, line))
            throw std::runtime_error("truncated NCNN param file: " + display_path(path));

        std::istringstream layer_stream(line);
        std::string type;
        std::string name;
        int bottom_count{};
        int top_count{};
        if (!(layer_stream >> type >> name >> bottom_count >> top_count) ||
            bottom_count < 0 || top_count < 0 ||
            static_cast<std::size_t>(bottom_count) > blob_count ||
            static_cast<std::size_t>(top_count) > blob_count)
        {
            throw std::runtime_error("invalid NCNN layer record: " + display_path(path));
        }

        std::vector<std::string> bottoms(static_cast<std::size_t>(bottom_count));
        for (auto& bottom : bottoms)
        {
            if (!(layer_stream >> bottom))
                throw std::runtime_error("invalid NCNN bottom list: " + display_path(path));
        }
        for (int i = 0; i < top_count; ++i)
        {
            std::string ignored_top;
            if (!(layer_stream >> ignored_top))
                throw std::runtime_error("invalid NCNN top list: " + display_path(path));
        }

        if (type != "InnerProduct" ||
            std::find(bottoms.begin(), bottoms.end(), "in0") == bottoms.end())
        {
            continue;
        }

        std::size_t output_size{};
        std::size_t weight_count{};
        for (std::string parameter; layer_stream >> parameter;)
        {
            if (parameter.starts_with("0="))
                output_size = parse_positive_size(std::string_view(parameter).substr(2), path, "output size");
            else if (parameter.starts_with("2="))
                weight_count = parse_positive_size(std::string_view(parameter).substr(2), path, "weight count");
        }

        if (output_size == 0 || weight_count == 0 || weight_count % output_size != 0)
            throw std::runtime_error("invalid input InnerProduct dimensions: " + display_path(path));
        return weight_count / output_size;
    }

    throw std::runtime_error("NCNN model has no InnerProduct layer consuming 'in0': " + display_path(path));
}
