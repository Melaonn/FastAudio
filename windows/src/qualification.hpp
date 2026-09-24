#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace fastaudio {

struct ArrivalEvent {
    int64_t arrivalNs;
    uint32_t frames;
};

struct CandidateResult {
    double targetMs = 0;
    uint64_t concealedFrames = 0;
    uint32_t hardUnderruns = 0;
    uint32_t maxDeficitFrames = 0;
};

struct QualificationResult {
    bool accepted = false;
    double targetMs = 0;
    double estimatedLatencyMs = 0;
    std::string grade = "Unsupported";
    CandidateResult details;
};

inline double minimumBurstReserveMs(
        bool legacy, bool shared, uint32_t bufferFrames,
        uint32_t burstFrames, uint32_t sampleRate = 48000) {
    if (legacy || !shared || bufferFrames <= burstFrames) {
        return 0;
    }
    return (bufferFrames - burstFrames) * 1000.0 / sampleRate;
}

inline CandidateResult simulateCandidate(
        const std::vector<ArrivalEvent>& events, double targetMs,
        uint32_t sampleRate = 48000) {
    CandidateResult result;
    result.targetMs = targetMs;
    if (events.empty()) {
        result.hardUnderruns = 1;
        return result;
    }

    const int64_t playStart = events.front().arrivalNs
            + static_cast<int64_t>(targetMs * 1'000'000.0);
    const double queueCeiling = targetMs * sampleRate / 1000.0;
    int64_t lastTime = playStart;
    double queued = 0;
    bool started = false;
    uint64_t totalConsumed = 0;
    const uint32_t concealableFrames = sampleRate * 3 / 1000;

    for (const auto& event : events) {
        if (event.arrivalNs < playStart) {
            queued = std::min(queueCeiling, queued + event.frames);
            continue;
        }

        if (!started) {
            started = true;
            lastTime = playStart;
        }

        double required = static_cast<double>(event.arrivalNs - lastTime)
                * sampleRate / 1'000'000'000.0;
        totalConsumed += static_cast<uint64_t>(std::max(0.0, required));
        if (required > queued) {
            uint32_t deficit = static_cast<uint32_t>(std::ceil(required - queued));
            result.maxDeficitFrames = std::max(result.maxDeficitFrames, deficit);
            if (deficit > concealableFrames) {
                ++result.hardUnderruns;
            } else {
                result.concealedFrames += deficit;
            }
            queued = 0;
        } else {
            queued -= required;
        }
        queued = std::min(queueCeiling, queued + event.frames);
        lastTime = event.arrivalNs;
    }

    if (totalConsumed
            && static_cast<double>(result.concealedFrames) / totalConsumed >= 0.001) {
        ++result.hardUnderruns;
    }
    return result;
}

inline QualificationResult qualify(
        const std::vector<ArrivalEvent>& events, double renderPeriodMs = 0) {
    QualificationResult result;
    for (int halfMs = 16; halfMs <= 80; ++halfMs) {
        double candidateMs = halfMs * 0.5;
        CandidateResult candidate = simulateCandidate(events, candidateMs);
        if (!candidate.hardUnderruns) {
            result.accepted = true;
            result.targetMs = std::min(40.0, candidateMs + 1.0);
            result.estimatedLatencyMs = result.targetMs + renderPeriodMs;
            result.details = candidate;
            result.grade = result.estimatedLatencyMs <= 24.0
                    ? "Competitive"
                    : result.estimatedLatencyMs <= 35.0
                            ? "Standard" : "Unsupported";
            if (result.grade == "Unsupported") {
                result.accepted = false;
            }
            return result;
        }
    }
    return result;
}

// Playback-capture HALs commonly deliver a complete period in one burst. The
// original qualifier capped the queue at the requested reserve and therefore
// discarded most of every burst in the simulation. This model keeps one
// source burst plus the requested source-ring reserve. The endpoint period is
// not counted as free reserve because runtime prefill moves that period out of
// the ring before playback starts.
inline CandidateResult simulateBurstCandidate(
        const std::vector<ArrivalEvent>& events, double reserveMs,
        uint32_t burstFrames,
        uint32_t sampleRate = 48000) {
    CandidateResult result;
    result.targetMs = reserveMs;
    if (events.empty() || !burstFrames) {
        result.hardUnderruns = 1;
        return result;
    }

    const double reserveFrames = reserveMs * sampleRate / 1000.0;
    const double queueCeiling = burstFrames + reserveFrames;
    const int64_t playStart = events.front().arrivalNs
            + static_cast<int64_t>(reserveMs * 1'000'000.0);
    double queued = 0;
    int64_t lastTime = playStart;
    uint64_t totalConsumed = 0;
    const uint32_t concealableFrames = sampleRate * 3 / 1000;

    for (const auto& event : events) {
        if (event.arrivalNs <= playStart) {
            queued = std::min(queueCeiling, queued + event.frames);
            continue;
        }

        double required = static_cast<double>(event.arrivalNs - lastTime)
                * sampleRate / 1'000'000'000.0;
        totalConsumed += static_cast<uint64_t>(std::max(0.0, required));
        if (required > queued) {
            uint32_t deficit = static_cast<uint32_t>(std::ceil(required - queued));
            result.maxDeficitFrames = std::max(result.maxDeficitFrames, deficit);
            if (deficit > concealableFrames) {
                ++result.hardUnderruns;
            } else {
                result.concealedFrames += deficit;
            }
            queued = 0;
        } else {
            queued -= required;
        }
        queued = std::min(queueCeiling, queued + event.frames);
        lastTime = event.arrivalNs;
    }

    if (totalConsumed
            && static_cast<double>(result.concealedFrames) / totalConsumed >= 0.001) {
        ++result.hardUnderruns;
    }
    return result;
}

inline QualificationResult qualifyBurst(
        const std::vector<ArrivalEvent>& events, double endpointLatencyMs,
        uint32_t burstFrames,
        double minimumReserveMs = 2.0,
        uint32_t sampleRate = 48000) {
    QualificationResult result;
    const int firstHalfMs = std::max(
            4, static_cast<int>(std::ceil(minimumReserveMs * 2.0)));
    for (int halfMs = firstHalfMs; halfMs <= 80; ++halfMs) {
        double candidateMs = halfMs * 0.5;
        CandidateResult candidate = simulateBurstCandidate(
                events, candidateMs, burstFrames, sampleRate);
        if (!candidate.hardUnderruns) {
            result.accepted = true;
            result.targetMs = std::min(40.0, candidateMs + 1.0);
            result.estimatedLatencyMs = burstFrames * 1000.0 / sampleRate
                    + result.targetMs + endpointLatencyMs;
            result.details = candidate;
            result.grade = result.estimatedLatencyMs <= 24.0
                    ? "Competitive"
                    : result.estimatedLatencyMs <= 35.0
                            ? "Standard" : "Unsupported";
            if (result.grade == "Unsupported") {
                result.accepted = false;
            }
            return result;
        }
    }
    return result;
}

}  // namespace fastaudio
