#include "pch.h"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "Core/FileAbstractionLayer.h"
#include "Core/LocaleConverter.h"
#include "Lyric/MLPipeline/MLPipelineCommon.h"
#include "Lyric/MLPipeline/NCNNPipeline.h"
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4244 4251 4267)
#endif
#include <ncnn/datareader.h>
#include <ncnn/net.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

namespace
{
    class BoundedMemoryDataReader final : public ncnn::DataReader
    {
    public:
        explicit BoundedMemoryDataReader(const std::span<const unsigned char> data)
            : current_(data.data()), end_(data.data() + data.size())
        {
        }

        std::size_t read(void* buffer, const std::size_t size) const override
        {
            const auto available = static_cast<std::size_t>(end_ - current_);
            const auto bytes_to_read = std::min(size, available);
            if (bytes_to_read > 0)
            {
                std::memcpy(buffer, current_, bytes_to_read);
                current_ += bytes_to_read;
            }
            return bytes_to_read;
        }

        std::size_t reference(const std::size_t size, const void** buffer) const override
        {
            const auto available = static_cast<std::size_t>(end_ - current_);
            if (buffer == nullptr || size > available)
                return 0;

            *buffer = current_;
            current_ += size;
            return size;
        }

    private:
        mutable const unsigned char* current_;
        const unsigned char* end_;
    };

    std::string display_path(const std::wstring& path)
    {
        const auto utf8_path = MusicPlayerLibrary::LocaleConverter::GetUtf8StringFromUtf16String(path);
        return utf8_path.empty() && !path.empty() ? "<unrepresentable path>" : utf8_path;
    }

    std::vector<unsigned char> read_binary_file(
        const std::wstring& path,
        const std::string_view description)
    {
        auto file = MusicPlayerLibrary::GetDefaultFileSystem().OpenReadFile(path, true, true);
        if (!file)
        {
            throw std::runtime_error(
                "failed to open " + std::string(description) + ": " + display_path(path));
        }

        const auto length = file->GetLength();
        if (length == 0)
            throw std::runtime_error(std::string(description) + " is empty: " + display_path(path));

        std::vector<unsigned char> contents;
        if (length > static_cast<std::uint64_t>(contents.max_size()))
            throw std::runtime_error(std::string(description) + " is too large: " + display_path(path));

        contents.resize(static_cast<std::size_t>(length));
        MusicPlayerLibrary::MLPipeline::read_exact(
            *file,
            reinterpret_cast<char*>(contents.data()),
            contents.size(),
            path);
        return contents;
    }
}

namespace MusicPlayerLibrary::MLPipeline
{

    std::uint64_t fingerprint_ncnn_model(const NcnnModelFiles& files)
    {
        std::uint64_t fingerprint = fnv_offset_basis;
        constexpr std::string_view domain = "WMP-NCNN-MODEL-V1";
        fingerprint_bytes(fingerprint, domain.data(), domain.size());
        fingerprint_file(fingerprint, "param", files.param);
        fingerprint_file(fingerprint, "weights", files.weights);
        return fingerprint;
    }

    class NcnnClassifier::Impl
    {
    public:
        explicit Impl(const NcnnModelFiles& files)
            : input_size(read_ncnn_input_size(files.param)),
              model_fingerprint(fingerprint_ncnn_model(files)),
              model_data(read_binary_file(files.weights, "NCNN weights file"))
        {
            auto param_data = read_binary_file(files.param, "NCNN param file");
            param_data.push_back('\0');

            net.opt.use_vulkan_compute = true;
            if (net.load_param_mem(reinterpret_cast<const char*>(param_data.data())) != 0)
                throw std::runtime_error("failed to load NCNN param file: " + display_path(files.param));

            const BoundedMemoryDataReader model_reader(model_data);
            if (net.load_model(model_reader) != 0)
                throw std::runtime_error("failed to load NCNN weights file: " + display_path(files.weights));
            if (!vk_checked)
            {
                const ncnn::VulkanDevice* vkdev = net.vulkan_device();
                if (vkdev && vkdev->is_valid()) {
                    NATIVE_TRACE("info: NCNN vulkan device is available for interference");
                    NATIVE_TRACE("info: vulkan device=%s, implementer=%s, implementation version=%u", vkdev->info.device_name(), vkdev->info.driver_name(), vkdev->info.driver_version());
                }
                else
                {
                    NATIVE_TRACE("info: NCNN vulkan device is not available, fallback to CPU interference");
                }
                vk_checked = true;
            }
        }

        std::size_t input_size;
        std::uint64_t model_fingerprint;
        // NCNN references model data instead of copying it, so this buffer must outlive net.
        std::vector<unsigned char> model_data;
        ncnn::Net net;
        static bool vk_checked;
    };
    
    bool NcnnClassifier::Impl::vk_checked = false;

    NcnnClassifier::NcnnClassifier(const NcnnModelFiles& files)
        : impl_(std::make_unique<Impl>(files))
    {
    }

    NcnnClassifier::~NcnnClassifier() = default;
    NcnnClassifier::NcnnClassifier(NcnnClassifier&&) noexcept = default;
    NcnnClassifier& NcnnClassifier::operator=(NcnnClassifier&&) noexcept = default;

    std::size_t NcnnClassifier::input_size() const noexcept
    {
        return impl_->input_size;
    }

    std::uint64_t NcnnClassifier::model_fingerprint() const noexcept
    {
        return impl_->model_fingerprint;
    }

    std::vector<float> NcnnClassifier::run(const std::span<const float> features) const
    {
        if (features.size() != impl_->input_size)
        {
            throw std::invalid_argument(
                "NCNN input has " + std::to_string(features.size()) +
                " features; expected " + std::to_string(impl_->input_size));
        }

        ncnn::Mat input(static_cast<int>(features.size()));
        std::memcpy(input.data, features.data(), features.size_bytes());

        auto extractor = impl_->net.create_extractor();
        if (extractor.input("in0", input) != 0)
            throw std::runtime_error("failed to set NCNN input blob 'in0'");

        ncnn::Mat output;
        if (extractor.extract("out0", output) != 0)
            throw std::runtime_error("failed to extract NCNN output blob 'out0'");

        const auto* begin = static_cast<const float*>(output.data);
        return { begin, begin + output.total() };
    }

    int NcnnClassifier::predict(const std::span<const float> features) const
    {
        const auto logits = run(features);
        return static_cast<int>(std::distance(logits.begin(), std::ranges::max_element(logits)));
    }
}
