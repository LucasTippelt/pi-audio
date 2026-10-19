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

void StateWriter::setConfigPath(const char* path) {
    m_configPath = path;
}

/* Find "key": in JSON and return pointer past the colon+whitespace. */
static const char* findJsonKey(const char* json, size_t len, const char* key) {
    char search[64];
    std::snprintf(search, sizeof(search), "\"%s\":", key);
    size_t klen = std::strlen(search);
    for (size_t i = 0; i + klen <= len; ++i) {
        if (std::memcmp(json + i, search, klen) == 0) {
            const char* p = json + i + klen;
            while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
            return p;
        }
    }
    return nullptr;
}

/* Extract a JSON string value into outBuf. Returns true on success. */
static bool extractJsonString(const char* json, size_t len, const char* key,
                               char* outBuf, size_t bufSize) {
    const char* p = findJsonKey(json, len, key);
    if (!p || *p != '"') return false;
    ++p; /* skip opening quote */
    size_t j = 0;
    while (*p && *p != '"' && j < bufSize - 1) {
        if (*p == '\\' && *(p + 1)) { ++p; } /* skip escape prefix, store char */
        outBuf[j++] = *p++;
    }
    outBuf[j] = '\0';
    return true;
}

/* Extract a JSON number (int or float) for a given key. Returns true on success. */
static bool extractJsonNumber(const char* json, size_t len, const char* key, double* out) {
    const char* p = findJsonKey(json, len, key);
    if (!p || *p == 'n') return false; /* null */
    char* end = nullptr;
    double val = strtod(p, &end);
    if (end > p) { *out = val; return true; }
    return false;
}

/* Extract a JSON boolean for a given key. Returns true on success. */
static bool extractJsonBool(const char* json, size_t len, const char* key, bool* out) {
    const char* p = findJsonKey(json, len, key);
    if (!p) return false;
    if (std::strncmp(p, "true", 4) == 0) { *out = true; return true; }
    if (std::strncmp(p, "false", 5) == 0) { *out = false; return true; }
    return false;
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

    /* Read config fields from current.json (input/output/volume/muted). */
    char cfgInput[128] = "";
    char cfgOutput[128] = "";
    double cfgVolume = 0;
    bool cfgMuted = false;
    bool hasInput = false, hasOutput = false, hasVolume = false, hasMuted = false;

    if (m_configPath) {
        FILE* cf = std::fopen(m_configPath, "r");
        if (cf) {
            fseek(cf, 0, SEEK_END);
            long csz = ftell(cf);
            fseek(cf, 0, SEEK_SET);
            if (csz > 0 && csz < 65536) {
                char* cbuf = static_cast<char*>(malloc(static_cast<size_t>(csz) + 1));
                if (cbuf) {
                    std::fread(cbuf, 1, static_cast<size_t>(csz), cf);
                    cbuf[csz] = '\0';

                    hasInput = extractJsonString(cbuf, static_cast<size_t>(csz), "input", cfgInput, sizeof(cfgInput));
                    hasOutput = extractJsonString(cbuf, static_cast<size_t>(csz), "output", cfgOutput, sizeof(cfgOutput));
                    hasVolume = extractJsonNumber(cbuf, static_cast<size_t>(csz), "volume", &cfgVolume);
                    hasMuted = extractJsonBool(cbuf, static_cast<size_t>(csz), "muted", &cfgMuted);

                    free(cbuf);
                }
            }
            std::fclose(cf);
        }
    }

    FILE* f = std::fopen(m_statePath, "w");
    if (!f) return;

    /* Write JSON state compatible with pi-audio-status adapter.
     * connected is true because writeState() is only called when live
     * telemetry arrives; the web status adapter requires this field. */
    std::fprintf(f, "{\n");
    std::fprintf(f, "  \"connected\": true,\n");

    /* Config fields from current.json for debug comparison table. */
    if (hasInput) {
        std::fputs("  \"input\": ", f); writeJsonString(f, cfgInput); std::fputc(',', f); std::fputc('\n', f);
    } else {
        std::fprintf(f, "  \"input\": null,\n");
    }
    if (hasOutput) {
        std::fputs("  \"output\": ", f); writeJsonString(f, cfgOutput); std::fputc(',', f); std::fputc('\n', f);
    } else {
        std::fprintf(f, "  \"output\": null,\n");
    }
    if (hasVolume) {
        /* Volume is stored as integer percentage in config. */
        std::fprintf(f, "  \"volume\": %d,\n", static_cast<int>(cfgVolume));
    } else {
        std::fprintf(f, "  \"volume\": null,\n");
    }
    if (hasMuted) {
        std::fprintf(f, "  \"muted\": %s,\n", cfgMuted ? "true" : "false");
    } else {
        std::fprintf(f, "  \"muted\": null,\n");
    }

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
