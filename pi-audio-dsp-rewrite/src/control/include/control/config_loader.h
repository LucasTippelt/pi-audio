#pragma once
#include <cstdint>

/* ------------------------------------------------------------------ */
/*  Config loader — JSON to binary parameter conversion (E3)          */
/* ------------------------------------------------------------------ */

struct CmdLoadParams;

class ConfigLoader {
public:
    /* Parse JSON config file, validate parameters, precompute coefficients.
     * Outputs versioned binary parameter block in outParams.
     * Returns true on success. */
    bool load(const char* jsonPath, CmdLoadParams& outParams);

    /* Validate a single gain value. */
    static bool validateGain(float gain);

    /* Precompute IIR coefficients from JSON-like params. */
    static bool precomputeIirCoeffs(float cutoffHz, float sampleRate,
                                    float coeffs[], uint32_t coeffCount);

private:
    uint32_t m_paramVersion = 1;
};
