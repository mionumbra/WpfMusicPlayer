#include "pch.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>
#include "Core/FileAbstractionLayer.h"
#include "Core/LocaleConverter.h"
#include "Lyric/MLPipeline/VocabularyIO.h"
#include "Lyric/MLPipeline/MLPipelineCommon.h"

namespace MusicPlayerLibrary::MLPipeline
{
    Vocabulary load_vocab(
        const std::wstring& path,
        const std::uint64_t expected_model_fingerprint)
    {
        auto input = GetDefaultFileSystem().OpenReadFile(path, true, true);
        if (!input)
            throw std::runtime_error(
                "failed to open vocabulary: " + LocaleConverter::GetUtf8StringFromUtf16String(path));

        std::array<char, format_magic.size()> magic{};
        read_exact(*input, magic.data(), magic.size(), path);
        if (magic != format_magic)
            throw format_error(path, "unrecognized magic");

        const auto version = read_u32_le(*input, path);
        if (version != format_version)
            throw format_error(path, "unsupported version " + std::to_string(version));

        const auto entry_count = read_u32_le(*input, path);
        if (entry_count == 0 || entry_count > static_cast<std::uint32_t>(std::numeric_limits<int>::max()))
            throw format_error(path, "invalid entry count");
        const auto stored_bundle_fingerprint = read_u64_le(*input, path);

        const auto file_size = input->GetLength();
        if (file_size < format_header_size ||
            entry_count > (file_size - format_header_size) / (minimum_token_size + 1))
        {
            throw format_error(path, "entry count exceeds the file size");
        }

        Vocabulary vocabulary;
        vocabulary.reserve(entry_count);
        auto actual_bundle_fingerprint =
            begin_bundle_fingerprint(expected_model_fingerprint, entry_count);
        for (std::uint32_t index = 0; index < entry_count; ++index)
        {
            char length_byte{};
            read_exact(*input, &length_byte, 1, path);
            const auto token_size = static_cast<unsigned char>(length_byte);
            if (token_size < minimum_token_size || token_size > maximum_token_size)
                throw format_error(path, "token length must be between 1 and 3 bytes");

            std::string token(token_size, '\0');
            read_exact(*input, token.data(), token.size(), path);
            fingerprint_token(actual_bundle_fingerprint, token);
            if (!vocabulary.emplace(std::move(token), static_cast<int>(index)).second)
                throw format_error(path, "duplicate token");
        }

        if (input->GetPosition() != file_size)
            throw format_error(path, "unexpected trailing data");
        if (actual_bundle_fingerprint != stored_bundle_fingerprint)
            throw format_error(path, "model/vocabulary fingerprint mismatch");

        return vocabulary;
    }
}
