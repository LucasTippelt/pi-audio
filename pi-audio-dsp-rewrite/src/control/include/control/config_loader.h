#pragma once
#include <cstddef>
#include <cstdint>

/* ------------------------------------------------------------------ */
/*  Config loader — JSON to binary parameter conversion (E3)          */
/*  Full schema: bands, drivers, limiter, measurement.                */
/*  All JSON parsing stays in control plane; DSP core gets binary.    */
/* ------------------------------------------------------------------ */

struct CmdLoadParams;

/* Structured error codes for validation failures. */
enum ConfigError : uint32_t {
    CONFIG_OK              = 0,
    CONFIG_ERR_FILE        = 1,  /* cannot open/read file            */
    CONFIG_ERR_PARSE       = 2,  /* invalid JSON syntax              */
    CONFIG_ERR_SAMPLE_RATE = 3,  /* sample rate out of range         */
    CONFIG_ERR_BUFFER_SIZE = 4,  /* buffer size not power of 2       */
    CONFIG_ERR_GAIN        = 5,  /* gain outside [0, 1]             */
    CONFIG_ERR_FREQ_NYQUIST = 6, /* frequency above Nyquist          */
    CONFIG_ERR_DELAY       = 7,  /* negative delay                   */
    CONFIG_ERR_BANDS       = 8,  /* invalid band count or overlap    */
    CONFIG_ERR_LIMITER     = 9,  /* invalid limiter params           */
};

/* Full configuration schema (parsed from JSON). */
struct FullConfig {
    /* Audio format. */
    uint32_t sampleRate;
    uint32_t bufferSize;

    /* Gain stages. */
    float    inputGain[8];   /* per-channel input gain [0, 1]       */
    float    outputGain[8];  /* per-channel output gain [0, 1]      */
    uint32_t inputGainCount;
    uint32_t outputGainCount;

    /* Filter bands (IIR biquad coefficients — precomputed). */
    struct BandCoeffs {
        float b0, b1, b2; /* numerator                              */
        float a1, a2;     /* denominator (normalized by a0)         */
        uint32_t targetChannel;
    };
    static constexpr uint32_t kMaxBands = 16;
    BandCoeffs bands[kMaxBands];
    uint32_t bandCount;

    /* Driver settings. */
    struct DriverConfig {
        float delayMs;     /* delay in milliseconds                  */
        float trimDb;      /* level adjustment in dB                 */
        uint8_t phaseInvert; /* 0 or 1                               */
        char name[32];
    };
    static constexpr uint32_t kMaxDrivers = 8;
    DriverConfig drivers[kMaxDrivers];
    uint32_t driverCount;

    /* Limiter settings. */
    float limiterThresholdDb;
    float limiterAttackMs;
    float limiterReleaseMs;
    float limiterRatio;
    bool  limiterEnabled;

    /* Measurement response points (frequency -> gain dB). */
    struct MeasPoint {
        float frequencyHz;
        float gainDb;
    };
    static constexpr uint32_t kMaxMeasPoints = 64;
    MeasPoint measPoints[kMaxMeasPoints];
    uint32_t measPointCount;

    /* ALSA device names. */
    char captureDevice[128];
    char playbackDevice[128];

    /* Room correction ALSA device names (optional, empty = disabled). */
    char rcInputDevice[128];
    char rcOutputDevice[128];

    /* P4: Crossover configuration per output chain. */
    struct CrossoverConfig {
        float frequencyHz;     /* crossover frequency in Hz              */
        char  topologyName[32];/* "Linkwitz-Riley", "Butterworth", etc. */
        int   slopeDbOct;      /* 12, 18, 24, 36, or 48                 */
        bool  enabled;
    };
    CrossoverConfig crossoverWoofer;
    CrossoverConfig crossoverTweeter;
};

class ConfigLoader {
public:
    /* Parse JSON config file, validate parameters, precompute coefficients.
     * Outputs versioned binary parameter block in outParams.
     * Returns true on success; setLastError() records failure reason. */
    bool load(const char* jsonPath, CmdLoadParams& outParams);

    /* Validate a FullConfig after parsing. Returns error code. */
    ConfigError validate(const FullConfig& cfg) const;

    /* Validate a single gain value. */
    static bool validateGain(float gain);

    /* Precompute one-pole IIR coefficients (legacy). */
    static bool precomputeIirCoeffs(float cutoffHz, float sampleRate,
                                    float coeffs[], uint32_t coeffCount);

    /* E3: Precompute biquad coefficients from frequency, Q, gain.
     * Uses standard DSP cookbook formulas. Returns true on success.  */
    static bool precomputeBiquadCoeffs(float cutoffHz, float sampleRate,
                                        float q, float gainDb,
                                        float* b0, float* b1, float* b2,
                                        float* a1, float* a2);

    /* Crossover coefficient precomputation (control plane only).
     * Supports Linkwitz-Riley, Butterworth, and Bessel topologies.
     * Outputs flat array of biquad coefficients: 5 floats per section (b0,b1,b2,a1,a2).
     * Caller must provide coeffs[] with space for at least 20 floats (4 sections × 5).
     * Returns true on success. */
    enum FilterType { FILTER_LOWPASS, FILTER_HIGHPASS };
    enum CrossoverTopology { TOPO_LR, TOPO_BW, TOPO_BESEL };

    static bool precomputeCrossoverCoeffs(
        float cutoffHz, uint32_t sampleRate,
        FilterType type, CrossoverTopology topology, int slopeDbOct,
        float coeffs[], uint32_t* numBiquadsOut, bool* hasFirstOrderOut,
        float* firstOrderCoefOut);

    /* Convert trim dB to linear gain factor. */
    static float dbToLinear(float db);

    /* Compute delay line size in samples from milliseconds. */
    static uint32_t delayMsToSamples(float delayMs, uint32_t sampleRate);

    /* Extract capture/playback device names from raw JSON. Writes to provided buffers (at least 128 bytes each). Returns true on success. */
    static bool parseDeviceNames(const char* json, size_t len,
                                   char capture[128], char playback[128]);

    /* Extract room correction device names from raw JSON. Writes to provided buffers (at least 128 bytes each). Empty = disabled. Returns true on success. */
    static bool parseRcDeviceNames(const char* json, size_t len,
                                     char rcInput[128], char rcOutput[128]);

    /* Extract volume (0-100) from JSON "volume" field, convert to linear gain [0, 1]. Returns true on success. */
    static bool extractVolumeGain(const char* json, size_t len, float* outGain);

    /* Extract muted state from JSON "muted" field. Returns true on success. */
    static bool extractMuted(const char* json, size_t len, bool* outMuted);

    /* Extract raw volume percentage (0-100) from JSON "volume" field. Returns true on success. */
    static int extractVolumePercent(const char* json, size_t len);

    /* Extract sample rate from JSON "sampleRate" field. Returns 0 if not found. */
    static uint32_t extractSampleRate(const char* json, size_t len);

    /* Extract buffer size from JSON "BufferSize" field. Returns 0 if not found. */
    static uint32_t extractBufferSize(const char* json, size_t len);

    /* Parse raw JSON into FullConfig struct (public wrapper around parseJson). */
    static bool parseFullConfig(const char* json, size_t len, FullConfig& cfg);

    /* Get last error code. Thread-unsafe; for single-threaded control plane. */
    ConfigError getLastError() const { return m_lastError; }

private:
    void setLastError(ConfigError err) { m_lastError = err; }

    /* Internal: parse JSON content into FullConfig. */
    bool parseJson(const char* json, size_t len, FullConfig& cfg);

    /* Internal: extract a float value from JSON by key path. */
    static bool jsonGetFloat(const char* json, size_t len, const char* key, float* out);

    /* Internal: extract an integer value from JSON by key path. */
    static bool jsonGetInt(const char* json, size_t len, const char* key, int64_t* out);

    uint32_t m_paramVersion = 1;
    ConfigError m_lastError = CONFIG_OK;
};
