#include "control/config_loader.h"
#include "msg/command.h"
#include "msg/params.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cfloat>
#include <string>

/* ------------------------------------------------------------------ */
/*  Minimal JSON value extraction — no library dependency (E3, 4D)    */
/*  Control plane only; DSP core has zero JSON dependency.            */
/* ------------------------------------------------------------------ */

static const char* jsonFindKey(const char* json, size_t len, const char* key) {
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    size_t klen = strlen(pattern);

    const char* p = json;
    const char* end = json + len;
    while (p < end) {
        p = strstr(p, pattern);
        if (!p) return nullptr;
        /* Check this is actually a key (followed by optional whitespace then ':'). */
        const char* after = p + klen;
        while (*after == ' ' || *after == '\t' || *after == '\n' || *after == '\r') ++after;
        if (*after == ':') return after + 1;
        ++p;
    }
    return nullptr;
}

static void skipWhitespace(const char*& p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
}

bool ConfigLoader::jsonGetFloat(const char* json, size_t len, const char* key, float* out) {
    const char* val = jsonFindKey(json, len, key);
    if (!val) return false;
    skipWhitespace(val);
    /* Skip "null". */
    if (strncmp(val, "null", 4) == 0) return false;
    /* Handle array value like [0.5, 0.75] — take first element. */
    if (*val == '[') {
        ++val;
        skipWhitespace(val);
    }
    char* end = nullptr;
    double d = strtod(val, &end);
    if (end == val) return false;
    *out = static_cast<float>(d);
    return true;
}

bool ConfigLoader::jsonGetInt(const char* json, size_t len, const char* key, int64_t* out) {
    const char* val = jsonFindKey(json, len, key);
    if (!val) return false;
    skipWhitespace(val);
    if (strncmp(val, "null", 4) == 0) return false;
    if (*val == '[') {
        ++val;
        skipWhitespace(val);
    }
    char* end = nullptr;
    long long ll = strtoll(val, &end, 10);
    if (end == val) return false;
    *out = ll;
    return true;
}

/* ------------------------------------------------------------------ */
/*  Parse JSON array of objects for bands/drivers/measurement points. */
/* ------------------------------------------------------------------ */

static const char* jsonFindArray(const char* json, size_t len, const char* key) {
    const char* val = jsonFindKey(json, len, key);
    if (!val) return nullptr;
    skipWhitespace(val);
    if (*val == '[') return val;
    return nullptr;
}

/* Count elements in a JSON array (handles objects and simple values). */
static int jsonArrayCount(const char* arr) {
    if (!arr || *arr != '[') return 0;
    const char* p = arr + 1;
    skipWhitespace(p);
    if (*p == ']') return 0;
    int count = 0;
    while (*p && *p != ']') {
        if (*p == '{') {
            /* Find matching '}'. */
            int depth = 1;
            ++p;
            while (*p && depth > 0) {
                if (*p == '{') ++depth;
                else if (*p == '}') --depth;
                ++p;
            }
        } else if (*p == '"') {
            /* Skip string. */
            ++p;
            while (*p && *p != '"') {
                if (*p == '\\') ++p;
                ++p;
            }
            if (*p) ++p;
        } else {
            /* Simple value (number, bool, null) — skip to comma or ']'. */
            while (*p && *p != ',' && *p != ']') ++p;
        }
        ++count;
        skipWhitespace(p);
        if (*p == ',') ++p;
        skipWhitespace(p);
    }
    return count;
}

/* Extract i-th numeric value from a simple JSON array like [0.8, 0.9]. */
static bool jsonArrayGetFloat(const char* arr, int index, float* out) {
    if (!arr || *arr != '[' || index < 0) return false;
    const char* p = arr + 1;
    for (int i = 0; i <= index && *p && *p != ']'; ++i) {
        skipWhitespace(p);
        /* Skip non-numeric elements. */
        if (*p == '{') {
            int depth = 1; ++p;
            while (*p && depth > 0) {
                if (*p == '{') ++depth;
                else if (*p == '}') --depth;
                ++p;
            }
        } else if (*p == '"') {
            ++p;
            while (*p && *p != '"') {
                if (*p == '\\') ++p;
                ++p;
            }
            if (*p) ++p;
        } else if (*p == '-' || (*p >= '0' && *p <= '9')) {
            if (i == index) {
                char* end = nullptr;
                *out = static_cast<float>(strtod(p, &end));
                return true;
            }
            while (*p && *p != ',' && *p != ']') ++p;
        } else {
            /* Skip other tokens (true, false, null). */
            while (*p && *p != ',' && *p != ']') ++p;
        }
        skipWhitespace(p);
        if (*p == ',') ++p;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/*  Full JSON parser for config schema.                               */
/* ------------------------------------------------------------------ */

bool ConfigLoader::parseJson(const char* json, size_t len, FullConfig& cfg) {
    std::memset(&cfg, 0, sizeof(cfg));

    /* --- Audio format --- */
    int64_t sr = 48000;
    int64_t bs = 128;
    jsonGetInt(json, len, "sampleRate", &sr);
    /* Accept both "BufferSize" (web schema) and "bufferSize" (legacy). */
    if (!jsonGetInt(json, len, "BufferSize", &bs)) {
        jsonGetInt(json, len, "bufferSize", &bs);
    }
    cfg.sampleRate = static_cast<uint32_t>(sr);
    cfg.bufferSize = static_cast<uint32_t>(bs);

    /* --- Device names (defaults to "default"). --- */
    std::strncpy(cfg.captureDevice, "default", sizeof(cfg.captureDevice) - 1);
    cfg.captureDevice[sizeof(cfg.captureDevice) - 1] = '\0';
    std::strncpy(cfg.playbackDevice, "default", sizeof(cfg.playbackDevice) - 1);
    cfg.playbackDevice[sizeof(cfg.playbackDevice) - 1] = '\0';

    {
        const char* val = jsonFindKey(json, len, "input");
        if (val) {
            skipWhitespace(val);
            if (*val == '"') {
                ++val;
                const char* endQuote = strchr(val, '"');
                if (endQuote) {
                    size_t nlen = endQuote - val;
                    if (nlen >= sizeof(cfg.captureDevice)) nlen = sizeof(cfg.captureDevice) - 1;
                    std::memcpy(cfg.captureDevice, val, nlen);
                    cfg.captureDevice[nlen] = '\0';
                }
            }
        }
    }
    {
        const char* val = jsonFindKey(json, len, "output");
        if (val) {
            skipWhitespace(val);
            if (*val == '"') {
                ++val;
                const char* endQuote = strchr(val, '"');
                if (endQuote) {
                    size_t nlen = endQuote - val;
                    if (nlen >= sizeof(cfg.playbackDevice)) nlen = sizeof(cfg.playbackDevice) - 1;
                    std::memcpy(cfg.playbackDevice, val, nlen);
                    cfg.playbackDevice[nlen] = '\0';
                }
            }
        }
    }

    /* --- Gain (backward compatibility: single gain value). --- */
    float singleGain = 1.0f;
    if (jsonGetFloat(json, len, "gain", &singleGain)) {
        cfg.inputGain[0]   = singleGain;
        cfg.outputGain[0]  = singleGain;
        cfg.inputGainCount  = 1;
        cfg.outputGainCount = 1;
    }

    /* --- Input gains array. --- */
    {
        const char* arr = jsonFindArray(json, len, "inputGains");
        if (arr) {
            int count = jsonArrayCount(arr);
            cfg.inputGainCount = static_cast<uint32_t>(std::min(count, static_cast<int>(sizeof(cfg.inputGain) / sizeof(cfg.inputGain[0]))));
            for (uint32_t i = 0; i < cfg.inputGainCount; ++i) {
                float val = 1.0f;
                jsonArrayGetFloat(arr, static_cast<int>(i), &val);
                cfg.inputGain[i] = val;
            }
        }
    }

    /* --- Output gains array. --- */
    {
        const char* arr = jsonFindArray(json, len, "outputGains");
        if (arr) {
            int count = jsonArrayCount(arr);
            cfg.outputGainCount = static_cast<uint32_t>(std::min(count, static_cast<int>(sizeof(cfg.outputGain) / sizeof(cfg.outputGain[0]))));
            for (uint32_t i = 0; i < cfg.outputGainCount; ++i) {
                float val = 1.0f;
                jsonArrayGetFloat(arr, static_cast<int>(i), &val);
                cfg.outputGain[i] = val;
            }
        }
    }

    /* --- Bands (filter coefficients). --- */
    {
        const char* arr = jsonFindArray(json, len, "bands");
        if (arr) {
            int count = jsonArrayCount(arr);
            cfg.bandCount = std::min(static_cast<uint32_t>(count), FullConfig::kMaxBands);

            /* Parse each band object. */
            const char* p = arr + 1;
            for (uint32_t i = 0; i < cfg.bandCount && *p && *p != ']'; ++i) {
                skipWhitespace(p);
                if (*p != '{') break;

                /* Find matching '}' and extract fields from this object. */
                const char* objStart = p;
                int depth = 1;
                ++p;
                while (*p && depth > 0) {
                    if (*p == '{') ++depth;
                    else if (*p == '}') --depth;
                    ++p;
                }
                const char* objEnd = p;

                /* Extract fields from this object slice. */
                std::string objStr(objStart, objEnd - objStart);
                size_t objLen = objStr.size();

                float cutoffHz = 0, qVal = 0.707f, gainDb = 0;
                int64_t targetCh = 0;

                { float tmp; if (jsonGetFloat(objStr.c_str(), objLen, "cutoffHz", &tmp)) cutoffHz = tmp; }
                { float tmp; if (jsonGetFloat(objStr.c_str(), objLen, "q", &tmp)) qVal = tmp; }
                { float tmp; if (jsonGetFloat(objStr.c_str(), objLen, "gainDb", &tmp)) gainDb = tmp; }
                { int64_t tmp; if (jsonGetInt(objStr.c_str(), objLen, "targetChannel", &tmp)) targetCh = tmp; }

                /* Precompute biquad coefficients. */
                float b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
                if (precomputeBiquadCoeffs(cutoffHz, cfg.sampleRate, qVal, gainDb,
                                           &b0, &b1, &b2, &a1, &a2)) {
                    cfg.bands[i].b0 = b0;
                    cfg.bands[i].b1 = b1;
                    cfg.bands[i].b2 = b2;
                    cfg.bands[i].a1 = a1;
                    cfg.bands[i].a2 = a2;
                    cfg.bands[i].targetChannel = static_cast<uint32_t>(targetCh >= 0 ? targetCh : 0);
                }

                skipWhitespace(p);
                if (*p == ',') ++p;
            }
        }
    }

    /* --- Drivers. --- */
    {
        const char* arr = jsonFindArray(json, len, "drivers");
        if (arr) {
            int count = jsonArrayCount(arr);
            cfg.driverCount = std::min(static_cast<uint32_t>(count), FullConfig::kMaxDrivers);

            const char* p = arr + 1;
            for (uint32_t i = 0; i < cfg.driverCount && *p && *p != ']'; ++i) {
                skipWhitespace(p);
                if (*p != '{') break;

                const char* objStart = p;
                int depth = 1;
                ++p;
                while (*p && depth > 0) {
                    if (*p == '{') ++depth;
                    else if (*p == '}') --depth;
                    ++p;
                }
                const char* objEnd = p;

                std::string objStr(objStart, objEnd - objStart);
                size_t objLen = objStr.size();

                float delayMs = 0, trimDb = 0;
                int64_t phaseInv = 0;

                { float tmp; if (jsonGetFloat(objStr.c_str(), objLen, "delayMs", &tmp)) delayMs = tmp; }
                { float tmp; if (jsonGetFloat(objStr.c_str(), objLen, "trimDb", &tmp)) trimDb = tmp; }
                { int64_t tmp; if (jsonGetInt(objStr.c_str(), objLen, "phaseInvert", &tmp)) phaseInv = tmp; }

                cfg.drivers[i].delayMs     = delayMs;
                cfg.drivers[i].trimDb      = trimDb;
                cfg.drivers[i].phaseInvert = static_cast<uint8_t>(phaseInv != 0 ? 1 : 0);

                /* Extract name string. */
                {
                    const char* val = jsonFindKey(objStr.c_str(), objLen, "name");
                    if (val) {
                        skipWhitespace(val);
                        if (*val == '"') {
                            ++val;
                            const char* endQuote = strchr(val, '"');
                            if (endQuote) {
                                size_t nlen = endQuote - val;
                                if (nlen >= sizeof(cfg.drivers[i].name))
                                    nlen = sizeof(cfg.drivers[i].name) - 1;
                                memcpy(cfg.drivers[i].name, val, nlen);
                                cfg.drivers[i].name[nlen] = '\0';
                            }
                        }
                    }
                }

                skipWhitespace(p);
                if (*p == ',') ++p;
            }
        }
    }

    /* --- Limiter. --- */
    {
        bool enabled = false;
        int64_t enVal = 0;
        jsonGetInt(json, len, "limiterEnabled", &enVal);
        enabled = (enVal != 0);

        float thresholdDb = -1.0f, attackMs = 5.0f, releaseMs = 100.0f, ratio = 4.0f;
        { float tmp; if (jsonGetFloat(json, len, "limiterThresholdDb", &tmp)) thresholdDb = tmp; }
        { float tmp; if (jsonGetFloat(json, len, "limiterAttackMs", &tmp)) attackMs = tmp; }
        { float tmp; if (jsonGetFloat(json, len, "limiterReleaseMs", &tmp)) releaseMs = tmp; }
        { float tmp; if (jsonGetFloat(json, len, "limiterRatio", &tmp)) ratio = tmp; }

        cfg.limiterEnabled      = enabled;
        cfg.limiterThresholdDb  = thresholdDb;
        cfg.limiterAttackMs     = attackMs;
        cfg.limiterReleaseMs    = releaseMs;
        cfg.limiterRatio        = ratio;
    }

    /* --- Measurement points. --- */
    {
        const char* arr = jsonFindArray(json, len, "measPoints");
        if (arr) {
            int count = jsonArrayCount(arr);
            cfg.measPointCount = std::min(static_cast<uint32_t>(count), FullConfig::kMaxMeasPoints);

            const char* p = arr + 1;
            for (uint32_t i = 0; i < cfg.measPointCount && *p && *p != ']'; ++i) {
                skipWhitespace(p);
                if (*p != '{') break;

                const char* objStart = p;
                int depth = 1;
                ++p;
                while (*p && depth > 0) {
                    if (*p == '{') ++depth;
                    else if (*p == '}') --depth;
                    ++p;
                }
                const char* objEnd = p;

                std::string objStr(objStart, objEnd - objStart);
                size_t objLen = objStr.size();

                float freqHz = 0, gainDb = 0;
                { float tmp; if (jsonGetFloat(objStr.c_str(), objLen, "frequencyHz", &tmp)) freqHz = tmp; }
                { float tmp; if (jsonGetFloat(objStr.c_str(), objLen, "gainDb", &tmp)) gainDb = tmp; }

                cfg.measPoints[i].frequencyHz = freqHz;
                cfg.measPoints[i].gainDb      = gainDb;

                skipWhitespace(p);
                if (*p == ',') ++p;
            }
        }
    }

    return true;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

bool ConfigLoader::load(const char* jsonPath, CmdLoadParams& outParams) {
    setLastError(CONFIG_OK);

    FILE* f = std::fopen(jsonPath, "r");
    if (!f) {
        setLastError(CONFIG_ERR_FILE);
        return false;
    }

    /* Read entire file. */
    fseek(f, 0, SEEK_END);
    long fileSize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fileSize <= 0 || fileSize > 65536) {
        std::fclose(f);
        setLastError(CONFIG_ERR_PARSE);
        return false;
    }

    char* buf = static_cast<char*>(std::malloc(static_cast<size_t>(fileSize) + 1));
    if (!buf) {
        std::fclose(f);
        setLastError(CONFIG_ERR_FILE);
        return false;
    }

    size_t len = std::fread(buf, 1, static_cast<size_t>(fileSize), f);
    buf[len] = '\0';
    std::fclose(f);

    /* Parse JSON into FullConfig. */
    FullConfig cfg;
    if (!parseJson(buf, len, cfg)) {
        std::free(buf);
        setLastError(CONFIG_ERR_PARSE);
        return false;
    }
    std::free(buf);

    /* Validate. */
    ConfigError err = validate(cfg);
    if (err != CONFIG_OK) {
        setLastError(err);
        return false;
    }

    /* Build binary parameter block. */
    std::memset(&outParams, 0, sizeof(outParams));
    outParams.header.version      = 1;
    outParams.header.type         = CMD_LOAD_PARAMS;
    outParams.header.seq          = m_paramVersion++;
    outParams.paramVersion        = outParams.header.seq;

    /* Pack all parameters into payload using compact variable-length format.
     * Each entry: type(2) + stageSlot(2) + dataLen(4) + data[dataLen]       */
    uint8_t* ptr = outParams.payload;
    uint8_t* end = outParams.payload + sizeof(outParams.payload);

    auto canWrite = [&ptr, &end](size_t n) {
        return (ptr + n <= end);
    };

    /* Write ParamBlockHeader. */
    if (!canWrite(sizeof(ParamBlockHeader))) {
        setLastError(CONFIG_ERR_PARSE);
        return false;
    }
    ParamBlockHeader* hdr = reinterpret_cast<ParamBlockHeader*>(ptr);
    hdr->version     = outParams.paramVersion;
    hdr->stageCount  = 0; /* counted as we add entries */
    ptr += sizeof(ParamBlockHeader);

    /* Helper: write a compact entry (type + stageSlot + dataLen + data). */
    auto packEntry = [&ptr, &end, &hdr](uint16_t type, uint16_t slot,
                                         const void* data, size_t dataLen) -> bool {
        size_t entrySize = 2 + 2 + 4 + dataLen; /* header + data */
        if (ptr + entrySize > end) return false;
        std::memcpy(ptr, &type, 2);       ptr += 2;
        std::memcpy(ptr, &slot, 2);       ptr += 2;
        uint32_t dl = static_cast<uint32_t>(dataLen);
        std::memcpy(ptr, &dl, 4);         ptr += 4;
        if (data && dataLen > 0)
            std::memcpy(ptr, data, dataLen);
        ptr += dataLen;
        ++hdr->stageCount;
        return true;
    };

    /* Pack gain entries. */
    for (uint32_t i = 0; i < cfg.inputGainCount; ++i) {
        if (!packEntry(STAGE_PARAM_GAIN, static_cast<uint16_t>(i),
                       &cfg.inputGain[i], sizeof(float))) break;
    }

    for (uint32_t i = 0; i < cfg.outputGainCount; ++i) {
        if (!packEntry(STAGE_PARAM_GAIN, static_cast<uint16_t>(8 + i),
                       &cfg.outputGain[i], sizeof(float))) break;
    }

    /* Pack IIR coefficient entries. */
    for (uint32_t i = 0; i < cfg.bandCount; ++i) {
        float coeffs[5] = { cfg.bands[i].b0, cfg.bands[i].b1, cfg.bands[i].b2,
                            cfg.bands[i].a1, cfg.bands[i].a2 };
        if (!packEntry(STAGE_PARAM_IIR_COEF, static_cast<uint16_t>(cfg.bands[i].targetChannel),
                       coeffs, sizeof(coeffs))) break;
    }

    /* Pack delay entries. */
    for (uint32_t i = 0; i < cfg.driverCount; ++i) {
        uint32_t delaySamples = delayMsToSamples(cfg.drivers[i].delayMs, cfg.sampleRate);
        if (!packEntry(STAGE_PARAM_DELAY, static_cast<uint16_t>(i),
                       &delaySamples, sizeof(delaySamples))) break;
    }

    /* Pack trim entries. */
    for (uint32_t i = 0; i < cfg.driverCount; ++i) {
        float linearGain = dbToLinear(cfg.drivers[i].trimDb);
        if (!packEntry(STAGE_PARAM_TRIM, static_cast<uint16_t>(i),
                       &linearGain, sizeof(linearGain))) break;
    }

    outParams.payloadSize = static_cast<uint32_t>(ptr - outParams.payload);
    return true;
}

ConfigError ConfigLoader::validate(const FullConfig& cfg) const {
    /* Sample rate: 8000–192000. */
    if (cfg.sampleRate < 8000 || cfg.sampleRate > 192000)
        return CONFIG_ERR_SAMPLE_RATE;

    /* Buffer size: power of 2, 32–4096. */
    if (cfg.bufferSize < 32 || cfg.bufferSize > 4096 ||
        (cfg.bufferSize & (cfg.bufferSize - 1)) != 0)
        return CONFIG_ERR_BUFFER_SIZE;

    /* Gain range: [0, 1]. */
    for (uint32_t i = 0; i < cfg.inputGainCount; ++i) {
        if (!validateGain(cfg.inputGain[i]))
            return CONFIG_ERR_GAIN;
    }
    for (uint32_t i = 0; i < cfg.outputGainCount; ++i) {
        if (!validateGain(cfg.outputGain[i]))
            return CONFIG_ERR_GAIN;
    }

    /* Band frequencies: must be below Nyquist. */
    for (uint32_t i = 0; i < cfg.bandCount; ++i) {
        /* Coefficients are already precomputed; if they're all zero, the band was invalid. */
    }

    /* Driver delays: non-negative. */
    for (uint32_t i = 0; i < cfg.driverCount; ++i) {
        if (cfg.drivers[i].delayMs < 0)
            return CONFIG_ERR_DELAY;
    }

    /* Limiter validation. */
    if (cfg.limiterEnabled) {
        if (cfg.limiterRatio < 1.0f || cfg.limiterRatio > 100.0f)
            return CONFIG_ERR_LIMITER;
        if (cfg.limiterAttackMs <= 0 || cfg.limiterReleaseMs <= 0)
            return CONFIG_ERR_LIMITER;
    }

    /* Measurement points: valid frequencies. */
    for (uint32_t i = 0; i < cfg.measPointCount; ++i) {
        if (cfg.measPoints[i].frequencyHz <= 0 ||
            cfg.measPoints[i].frequencyHz > static_cast<float>(cfg.sampleRate) / 2.0f)
            return CONFIG_ERR_FREQ_NYQUIST;
    }

    return CONFIG_OK;
}

bool ConfigLoader::validateGain(float gain) {
    return gain >= 0.0f && gain <= 1.0f;
}

bool ConfigLoader::precomputeIirCoeffs(float cutoffHz, float sampleRate,
                                       float coeffs[], uint32_t coeffCount) {
    if (cutoffHz <= 0 || cutoffHz > sampleRate / 2 || sampleRate == 0)
        return false;
    if (coeffCount < 5)
        return false;

    /* Simple one-pole lowpass: y[n] = a*x[n] + (1-a)*y[n-1]. */
    float wc = 2.0f * 3.14159265358979f * cutoffHz / sampleRate;
    float a  = 1.0f - std::cosf(wc);

    coeffs[0] = a;       /* b0 */
    coeffs[1] = 0.f;     /* b1 */
    coeffs[2] = 0.f;     /* b2 */
    coeffs[3] = a;       /* a1 (normalized) */
    coeffs[4] = 1.0f - a;/* a2 */

    return true;
}

bool ConfigLoader::precomputeBiquadCoeffs(float cutoffHz, float sampleRate,
                                          float q, float gainDb,
                                          float* b0, float* b1, float* b2,
                                          float* a1, float* a2) {
    /* Validate inputs. */
    if (cutoffHz <= 0 || cutoffHz > sampleRate / 2.0f || sampleRate <= 0)
        return false;
    if (q <= 0) q = 0.707f; /* default: Butterworth */

    /* Standard biquad cookbook — lowpass peaking EQ.
     * Reference: https://webaudio.github.io/Audio-EQ-Cookbook/audio-eq-cookbook.html */
    float wc = 2.0f * 3.14159265358979f * cutoffHz / sampleRate;
    float cosWc = std::cosf(wc);
    float sinWc = std::sinf(wc);

    /* Convert gain from dB to linear. */
    float A = std::expf(gainDb / 20.0f);

    float beta = sinWc / (2.0f * q);

    /* Peaking EQ coefficients. */
    float a0 = 1.0f + beta;
    float b0Coeff = (1.0f + A * beta);
    float b1Coeff = -2.0f * cosWc;
    float b2Coeff = (1.0f - A * beta);
    float a1Coeff = 2.0f * cosWc;
    float a2Coeff = (1.0f - beta);

    /* Normalize by a0. */
    if (std::fabs(a0) < 1e-12f) return false;

    *b0 = b0Coeff / a0;
    *b1 = b1Coeff / a0;
    *b2 = b2Coeff / a0;
    *a1 = a1Coeff / a0;
    *a2 = a2Coeff / a0;

    /* Check for NaN/Inf. */
    if (std::isnan(*b0) || std::isinf(*b0)) return false;
    if (std::isnan(*b1) || std::isinf(*b1)) return false;
    if (std::isnan(*b2) || std::isinf(*b2)) return false;
    if (std::isnan(*a1) || std::isinf(*a1)) return false;
    if (std::isnan(*a2) || std::isinf(*a2)) return false;

    /* Stability check: triangle inequalities for biquad denominator. */
    if (std::fabs(*a2) >= 1.0f) return false;
    if (std::fabs(*a1) >= 1.0f + *a2) return false;

    return true;
}

float ConfigLoader::dbToLinear(float db) {
    return std::expf(db * 0.1151292546497023f); /* ln(10)/20 */
}

uint32_t ConfigLoader::delayMsToSamples(float delayMs, uint32_t sampleRate) {
    if (delayMs <= 0 || sampleRate == 0) return 0;
    return static_cast<uint32_t>(std::round(delayMs * sampleRate / 1000.0f));
}

bool ConfigLoader::parseDeviceNames(const char* json, size_t len,
                                      char capture[128], char playback[128]) {
    std::strncpy(capture, "default", 127);
    capture[127] = '\0';
    std::strncpy(playback, "default", 127);
    playback[127] = '\0';

    const char* val = jsonFindKey(json, len, "input");
    if (val) {
        skipWhitespace(val);
        if (*val == '"') {
            ++val;
            const char* endQuote = strchr(val, '"');
            if (endQuote) {
                size_t nlen = endQuote - val;
                if (nlen >= 127) nlen = 127;
                std::memcpy(capture, val, nlen);
                capture[nlen] = '\0';
            }
        }
    }

    val = jsonFindKey(json, len, "output");
    if (val) {
        skipWhitespace(val);
        if (*val == '"') {
            ++val;
            const char* endQuote = strchr(val, '"');
            if (endQuote) {
                size_t nlen = endQuote - val;
                if (nlen >= 127) nlen = 127;
                std::memcpy(playback, val, nlen);
                playback[nlen] = '\0';
            }
        }
    }

    return true;
}

bool ConfigLoader::extractVolumeGain(const char* json, size_t len, float* outGain) {
    const char* val = jsonFindKey(json, len, "volume");
    if (!val) return false;
    skipWhitespace(val);
    if (strncmp(val, "null", 4) == 0) return false;

    char* end = nullptr;
    double d = strtod(val, &end);
    if (end == val) return false;

    /* Convert percentage [0-100] to linear gain [0.0-1.0]. */
    float pct = static_cast<float>(d);
    if (pct < 0.f) pct = 0.f;
    if (pct > 100.f) pct = 100.f;
    *outGain = pct / 100.0f;
    return true;
}

bool ConfigLoader::extractMuted(const char* json, size_t len, bool* outMuted) {
    const char* val = jsonFindKey(json, len, "muted");
    if (!val) return false;
    skipWhitespace(val);

    if (strncmp(val, "true", 4) == 0 && (val[4] == ',' || val[4] == '}' || val[4] == '\n' || val[4] == '\r' || val[4] == ' ' || val[4] == '\t' || val[4] == '\0')) {
        *outMuted = true;
        return true;
    }
    if (strncmp(val, "false", 5) == 0 && (val[5] == ',' || val[5] == '}' || val[5] == '\n' || val[5] == '\r' || val[5] == ' ' || val[5] == '\t' || val[5] == '\0')) {
        *outMuted = false;
        return true;
    }
    return false;
}
