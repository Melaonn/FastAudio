#pragma once

#include <cstdint>

namespace fastaudio {

constexpr uint32_t kHelloMagic = 0x46415544;
constexpr uint32_t kPacketMagic = 0x46415031;
constexpr uint16_t kProtocolVersion = 1;
constexpr uint32_t kSampleRate = 48000;
constexpr uint16_t kChannels = 2;
constexpr uint16_t kBitsPerSample = 16;
constexpr uint32_t kBytesPerFrame = 4;
constexpr uint32_t kLegacyPacketFrames = 128;
constexpr uint32_t kLowLatencyPacketFrames = 1024;
constexpr uint32_t kHelloSize = 32;
constexpr uint32_t kPacketHeaderSize = 32;
constexpr uint32_t kCapabilityVoice = 1;
constexpr uint32_t kCapabilityUidFilter = 2;
constexpr uint32_t kFlagTimestampFallback = 1;
constexpr uint32_t kFlagDiscontinuity = 2;

// Microphone audio travels on a separate, PC-to-Android socket. Keep the
// playback protocol stable so a microphone error can never corrupt it.
constexpr uint32_t kMicHelloMagic = 0x46414D48;   // FAMH
constexpr uint32_t kMicPacketMagic = 0x46414D50;  // FAMP
constexpr uint32_t kMicSampleRate = 48000;
constexpr uint16_t kMicChannels = 1;
constexpr uint16_t kMicBitsPerSample = 16;
constexpr uint32_t kMicBytesPerFrame = 2;
constexpr uint32_t kMicPacketFrames = 480;  // 10 ms
constexpr uint32_t kMicHelloSize = 32;
constexpr uint32_t kMicPacketHeaderSize = 32;
constexpr uint32_t kMicStatusReady = 1;
constexpr uint32_t kMicFlagDiscontinuity = 1;

struct Hello {
    uint32_t sampleRate = 0;
    uint16_t channels = 0;
    uint16_t bitsPerSample = 0;
    uint32_t packetFrames = 0;
    uint32_t sessionId = 0;
    uint32_t capabilities = 0;
};

struct PacketHeader {
    uint64_t sequence = 0;
    uint64_t captureTimeNs = 0;
    uint32_t frameCount = 0;
    uint32_t flags = 0;
};

struct MicHello {
    uint32_t sampleRate = 0;
    uint16_t channels = 0;
    uint16_t bitsPerSample = 0;
    uint32_t packetFrames = 0;
    uint32_t status = 0;
    uint32_t packageUid = 0;
};

inline uint16_t readU16(const uint8_t* p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

inline uint32_t readU32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24)
         | (static_cast<uint32_t>(p[1]) << 16)
         | (static_cast<uint32_t>(p[2]) << 8)
         | static_cast<uint32_t>(p[3]);
}

inline uint64_t readU64(const uint8_t* p) {
    return (static_cast<uint64_t>(readU32(p)) << 32) | readU32(p + 4);
}

inline void writeU16(uint8_t* p, uint16_t value) {
    p[0] = static_cast<uint8_t>(value >> 8);
    p[1] = static_cast<uint8_t>(value);
}

inline void writeU32(uint8_t* p, uint32_t value) {
    p[0] = static_cast<uint8_t>(value >> 24);
    p[1] = static_cast<uint8_t>(value >> 16);
    p[2] = static_cast<uint8_t>(value >> 8);
    p[3] = static_cast<uint8_t>(value);
}

inline void writeU64(uint8_t* p, uint64_t value) {
    writeU32(p, static_cast<uint32_t>(value >> 32));
    writeU32(p + 4, static_cast<uint32_t>(value));
}

}  // namespace fastaudio
