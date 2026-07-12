#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace MusicPlayerLibrary::MLPipeline
{
    struct DenseLayer
    {
        std::size_t input_size{};
        std::size_t output_size{};
        std::vector<float> weights;
        std::vector<float> biases;
    };

    struct NcnnModelFiles
    {
        std::wstring param;
        std::wstring weights;
    };

    // A non-cryptographic fingerprint used to reject accidentally mixed model files.
    std::uint64_t fingerprint_ncnn_model(const NcnnModelFiles& files);

    class NcnnClassifier
    {
    public:
        explicit NcnnClassifier(const NcnnModelFiles& files);
        ~NcnnClassifier();

        NcnnClassifier(const NcnnClassifier&) = delete;
        NcnnClassifier& operator=(const NcnnClassifier&) = delete;
        NcnnClassifier(NcnnClassifier&&) noexcept;
        NcnnClassifier& operator=(NcnnClassifier&&) noexcept;

        std::size_t input_size() const noexcept;
        std::uint64_t model_fingerprint() const noexcept;
        std::vector<float> run(std::span<const float> features) const;
        int predict(std::span<const float> features) const;

    private:
        class Impl;
        std::unique_ptr<Impl> impl_;
    };
}
