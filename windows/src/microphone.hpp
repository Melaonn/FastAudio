#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace fastaudio {

enum class MicSampleEncoding {
    PcmInteger,
    Float32,
};

inline float decodeMicSample(const uint8_t* source, uint32_t bytesPerSample,
                             MicSampleEncoding encoding) {
    if (encoding == MicSampleEncoding::Float32) {
        if (bytesPerSample != 4) {
            throw std::runtime_error("Unsupported floating-point microphone format");
        }
        float value = 0;
        std::memcpy(&value, source, sizeof(value));
        return std::clamp(value, -1.0f, 1.0f);
    }
    switch (bytesPerSample) {
        case 2: {
            int16_t value = static_cast<int16_t>(
                    static_cast<uint16_t>(source[0])
                    | (static_cast<uint16_t>(source[1]) << 8));
            return static_cast<float>(value) / 32768.0f;
        }
        case 3: {
            int32_t value = static_cast<int32_t>(source[0])
                    | (static_cast<int32_t>(source[1]) << 8)
                    | (static_cast<int32_t>(source[2]) << 16);
            if (value & 0x00800000) {
                value |= ~0x00ffffff;
            }
            return static_cast<float>(value) / 8388608.0f;
        }
        case 4: {
            int32_t value = static_cast<int32_t>(source[0])
                    | (static_cast<int32_t>(source[1]) << 8)
                    | (static_cast<int32_t>(source[2]) << 16)
                    | (static_cast<int32_t>(source[3]) << 24);
            return static_cast<float>(value) / 2147483648.0f;
        }
        default:
            throw std::runtime_error("Unsupported integer microphone format");
    }
}

inline std::vector<float> downmixMicrophone(const uint8_t* source,
                                             uint32_t frames,
                                             uint16_t channels,
                                             uint16_t blockAlign,
                                             MicSampleEncoding encoding) {
    if (!channels || blockAlign % channels != 0) {
        throw std::runtime_error("Invalid microphone channel layout");
    }
    const uint32_t bytesPerSample = blockAlign / channels;
    std::vector<float> mono;
    mono.reserve(frames);
    for (uint32_t frame = 0; frame < frames; ++frame) {
        const uint8_t* interleaved = source + static_cast<size_t>(frame) * blockAlign;
        float sum = 0;
        for (uint16_t channel = 0; channel < channels; ++channel) {
            sum += decodeMicSample(interleaved + channel * bytesPerSample,
                                   bytesPerSample, encoding);
        }
        mono.push_back(std::clamp(sum / channels, -1.0f, 1.0f));
    }
    return mono;
}

class LinearMonoResampler {
public:
    explicit LinearMonoResampler(uint32_t sourceRate, uint32_t targetRate = 48000)
        : sourceStep_(static_cast<double>(sourceRate) / targetRate) {
        if (!sourceRate || !targetRate) {
            throw std::runtime_error("Invalid microphone sample rate");
        }
    }

    void append(const std::vector<float>& source, std::vector<int16_t>& output) {
        pending_.insert(pending_.end(), source.begin(), source.end());
        while (position_ + 1.0 < pending_.size()) {
            const size_t index = static_cast<size_t>(position_);
            const double fraction = position_ - index;
            const float sample = pending_[index]
                    + static_cast<float>((pending_[index + 1] - pending_[index])
                            * fraction);
            output.push_back(static_cast<int16_t>(std::lround(
                    std::clamp(sample, -1.0f, 0.9999695f) * 32768.0f)));
            position_ += sourceStep_;
        }
        const size_t consumed = static_cast<size_t>(position_);
        if (consumed) {
            pending_.erase(pending_.begin(), pending_.begin() + consumed);
            position_ -= consumed;
        }
    }

private:
    double sourceStep_;
    double position_ = 0;
    std::vector<float> pending_;
};

}  // namespace fastaudio
