#include "control/state_writer.h"
#include "msg/telemetry.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>

void StateWriter::start(const char* statePath, uint32_t intervalMs) {
    m_statePath   = statePath;
    m_intervalMs  = intervalMs;
    m_running     = true;

    m_workerThread = std::thread([this]() {
        while (m_running.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(m_intervalMs));
            if (!m_running.load()) break;

            TelemetryBlock tel{};
            if (!m_snapshot.load(&tel)) continue;

            writeState(tel);
        }
    });
}

void StateWriter::stop() {
    m_running.store(false);
    if (m_workerThread.joinable())
        m_workerThread.join();
}

void StateWriter::pushTelemetry(const TelemetryBlock& tel) {
    m_snapshot.store(tel);
}

void StateWriter::ingestTelemetry(const TelemetryBlock& tel) {
    m_snapshot.store(tel);
}

void StateWriter::setAudioConfig(uint32_t sampleRate, uint32_t bufferSize) {
    m_sampleRate = sampleRate;
    m_bufferSize = bufferSize;
}

void StateWriter::setAppliedVolume(int volume) {
    m_appliedVolume = volume;
}

void StateWriter::setAppliedMuted(bool muted) {
    m_appliedMuted = muted;
}

void StateWriter::setAppliedDevices(const char* input, const char* output) {
    if (input)
        std::strncpy(m_appliedInput, input, sizeof(m_appliedInput) - 1);
    if (output)
        std::strncpy(m_appliedOutput, output, sizeof(m_appliedOutput) - 1);
}

/* Escape a JSON string for output: handles quotes and backslashes. */
static void writeJsonString(FILE* f, const char* s) {
    std::fputc('"', f);
    for (; *s; ++s) {
        if (*s == '"') std::fputs("\\\"", f);
        else if (*s == '\\') std::fputs("\\\\", f);
        else if (*s == '\n') std::fputs("\\n", f);
        else if (*s == '\r') std::fputs("\\r", f);
        else if (*s == '\t') std::fputs("\\t", f);
        else std::fputc(*s, f);
    }
    std::fputc('"', f);
}

void StateWriter::writeState(const TelemetryBlock& tel) {
    if (!m_statePath) return;

    FILE* f = std::fopen(m_statePath, "w");
    if (!f) return;

    /* Write JSON state compatible with pi-audio-status adapter.
     * connected is true because writeState() is only called when live
     * telemetry arrives; the web status adapter requires this field. */
    std::fprintf(f, "{\n");
    std::fprintf(f, "  \"connected\": true,\n");

    /* Applied config fields — actual values loaded into the DSP engine,
     * NOT read from current.json on every cycle. Updated by reload thread
     * after config is applied with a short delay for settling. */
    if (m_appliedInput[0]) {
        std::fputs("  \"input\": ", f); writeJsonString(f, m_appliedInput); std::fputc(',', f); std::fputc('\n', f);
    } else {
        std::fprintf(f, "  \"input\": null,\n");
    }
    if (m_appliedOutput[0]) {
        std::fputs("  \"output\": ", f); writeJsonString(f, m_appliedOutput); std::fputc(',', f); std::fputc('\n', f);
    } else {
        std::fprintf(f, "  \"output\": null,\n");
    }
    if (m_appliedVolume >= 0) {
        std::fprintf(f, "  \"volume\": %d,\n", m_appliedVolume);
    } else {
        std::fprintf(f, "  \"volume\": null,\n");
    }
    std::fprintf(f, "  \"muted\": %s,\n", m_appliedMuted ? "true" : "false");

    /* Telemetry fields. */
    std::fprintf(f, "  \"sampleRate\": %u,\n", m_sampleRate);
    std::fprintf(f, "  \"BufferSize\": %u,\n", m_bufferSize);
    std::fprintf(f, "  \"version\": %u,\n", tel.version);
    std::fprintf(f, "  \"seq\": %u,\n", tel.seq);
    std::fprintf(f, "  \"xrunUnderrun\": %u,\n", tel.xrunUnderrun);
    std::fprintf(f, "  \"xrunOverrun\": %u,\n", tel.xrunOverrun);
    std::fprintf(f, "  \"clockState\": %u,\n", tel.clockState);
    std::fprintf(f, "  \"outputLevelDb\": [%.2f, %.2f],\n",
                  tel.outputLevelDb[0], tel.outputLevelDb[1]);
    std::fprintf(f, "  \"latencyFrames\": %u,\n", tel.latencyFrames);
    std::fprintf(f, "  \"sampleIndex\": %llu\n", static_cast<unsigned long long>(tel.sampleIndex));
    std::fprintf(f, "}\n");

    std::fclose(f);
}

/* Snapshot implementation — atomic latest-wins pattern. */
void StateWriter::Snapshot::store(const TelemetryBlock& t) {
    uint32_t old = version.load(std::memory_order_relaxed);
    data = t;
    version.store(old + 1, std::memory_order_release);
}

bool StateWriter::Snapshot::load(TelemetryBlock* out) const {
    uint32_t v = version.load(std::memory_order_acquire);
    if (v == 0) return false;
    *out = data;
    // Verify no write was in progress during our read.
    return version.load(std::memory_order_acquire) == v;
}
