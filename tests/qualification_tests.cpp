#include <cassert>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

#include "../windows/src/microphone.hpp"
#include "../windows/src/protocol.hpp"
#include "../windows/src/qualification.hpp"
#include "../windows/src/ring_buffer.hpp"

using namespace fastaudio;

static std::vector<ArrivalEvent> steadyEvents(int64_t gapNs, int count,
                                               uint32_t frames) {
    std::vector<ArrivalEvent> events;
    int64_t now = 0;
    for (int i = 0; i < count; ++i) {
        events.push_back({now, frames});
        now += gapNs;
    }
    return events;
}
int main() {
    {
        auto events = steadyEvents(2'666'667, 4000, 128);
        auto result = qualify(events, 3.0);
        assert(result.accepted);
        assert(result.grade == "Competitive");
        assert(result.targetMs <= 10.0);
        assert(result.estimatedLatencyMs >= 12.0);
        assert(result.estimatedLatencyMs <= 13.0);
    }

    {
        auto events = steadyEvents(2'666'667, 4000, 128);
        for (size_t i = 300; i < events.size(); ++i) {
            events[i].arrivalNs += 18'000'000;
        }
        auto result = qualify(events, 3.0);
        assert(result.accepted);
        assert(result.targetMs >= 18.0);
        assert(result.targetMs <= 24.0);
        assert(result.estimatedLatencyMs >= 21.0);
        assert(result.estimatedLatencyMs <= 27.0);
    }

    {
        auto events = steadyEvents(21'333'333, 1000, 1024);
        auto result = qualifyBurst(events, 3.0, 1024);
        assert(result.accepted);
        assert(result.grade == "Standard");
        assert(result.targetMs == 3.0);
        assert(result.estimatedLatencyMs >= 27.0);
        assert(result.estimatedLatencyMs <= 28.0);
    }

    {
        auto events = steadyEvents(21'333'333, 1000, 1024);
        for (size_t i = 300; i < events.size(); ++i) {
            events[i].arrivalNs += 10'000'000;
        }
        auto result = qualifyBurst(events, 3.0, 1024);
        assert(result.accepted);
        assert(result.targetMs == 8.0);
        assert(result.estimatedLatencyMs <= 35.0);
    }

    {
        std::vector<ArrivalEvent> events;
        auto result = qualifyBurst(events, 3.0, 1024);
        assert(!result.accepted);
    }

    {
        auto events = steadyEvents(21'333'333, 1000, 1024);
        auto result = qualifyBurst(events, 3.0, 1024, 8.0);
        assert(result.accepted);
        assert(result.targetMs == 9.0);
    }

    {
        auto events = steadyEvents(21'333'333, 1000, 1024);
        for (size_t i = 300; i < events.size(); ++i) {
            events[i].arrivalNs += 10'000'000;
        }
        auto lowEndpoint = qualifyBurst(events, 3.0, 1024);
        auto highEndpoint = qualifyBurst(events, 10.0, 1024);
        assert(lowEndpoint.targetMs == highEndpoint.targetMs);
        assert(!highEndpoint.accepted);
        assert(highEndpoint.grade == "Unsupported");
        assert(highEndpoint.estimatedLatencyMs
                > lowEndpoint.estimatedLatencyMs + 6.0);
    }

    {
        assert(minimumBurstReserveMs(false, true, 738, 1024) == 0.0);
        assert(minimumBurstReserveMs(false, false, 2048, 1024) == 0.0);
        assert(minimumBurstReserveMs(true, true, 2048, 1024) == 0.0);
        double reserve = minimumBurstReserveMs(
                false, true, 2048, 1024);
        assert(reserve > 21.3 && reserve < 21.4);
    }

    {
        PcmRingBuffer ring(16);
        int16_t source[16] = {
            1, 2, 3, 4, 5, 6, 7, 8,
            9, 10, 11, 12, 13, 14, 15, 16
        };
        assert(ring.push(source, 8) == 8);
        int16_t target[16]{};
        assert(ring.pop(target, 8) == 8);
        for (int i = 0; i < 16; ++i) {
            assert(target[i] == source[i]);
        }
    }

    {
        // Interleaved 16-bit stereo is averaged into the mono PC-to-phone wire format.
        const std::array<uint8_t, 8> stereo = {
            0xff, 0x7f, 0x00, 0x80,  // +1.0 and -1.0
            0x00, 0x00, 0x00, 0x40   // 0.0 and +0.5
        };
        auto mono = downmixMicrophone(stereo.data(), 2, 2, 4,
                                      MicSampleEncoding::PcmInteger);
        assert(mono.size() == 2);
        assert(std::abs(mono[0]) < 0.001f);
        assert(mono[1] > 0.249f && mono[1] < 0.251f);
    }

    {
        // The linear resampler preserves a continuous stream across input buffers.
        LinearMonoResampler resampler(48'000);
        std::vector<int16_t> output;
        resampler.append({0.0f, 0.25f, 0.5f}, output);
        resampler.append({0.75f, 1.0f}, output);
        assert(output.size() == 4);
        assert(output[0] == 0);
        assert(output[1] > 8'000 && output[1] < 8'500);
        assert(output[3] > 24'000 && output[3] < 25'000);
    }

    {
        // A bounded mic queue never grows into seconds of stale speech.
        MonoPcmRingBuffer ring(4);
        int16_t source[] = {1, 2, 3, 4, 5};
        assert(ring.push(source, 5) == 4);
        int16_t target[4]{};
        assert(ring.pop(target, 4) == 4);
        for (int index = 0; index < 4; ++index) {
            assert(target[index] == source[index]);
        }
    }

    {
        std::array<uint8_t, 8> bytes{};
        writeU16(bytes.data(), 0x1234);
        writeU32(bytes.data() + 2, 0x89abcdef);
        writeU16(bytes.data() + 6, 0xbeef);
        assert(readU16(bytes.data()) == 0x1234);
        assert(readU32(bytes.data() + 2) == 0x89abcdef);
        assert(readU16(bytes.data() + 6) == 0xbeef);
    }

    std::cout << "FastAudio tests passed\n";
    return 0;
}
