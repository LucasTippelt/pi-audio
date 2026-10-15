#include "control/config_loader.h"
#include "msg/command.h"
#include "msg/params.h"

#include <cstdio>
#include <cstring>
#include <cmath>

bool ConfigLoader::load(const char* jsonPath, CmdLoadParams& outParams) {
    /* Minimal JSON parser for config. In production, use nlohmann/json or similar.
     * This stub reads a simple key-value format and produces binary params. */
    FILE* f = std::fopen(jsonPath, "r");
    if (!f) return false;

    char buf[4096];
    size_t len = std::fread(buf, 1, sizeof(buf) - 1, f);
    buf[len] = '\0';
    std::fclose(f);

    /* Parse gain from JSON-like content. */
    float gain = 1.0f;
    char* gainStr = std::strstr(buf, "\"gain\"");
    if (gainStr) {
        char* colon = std::strchr(gainStr, ':');
        if (colon) {
            gain = static_cast<float>(std::atof(colon + 1));
        }
    }

    if (!validateGain(gain))
        return false;

    /* Build binary parameter block. */
    std::memset(&outParams, 0, sizeof(outParams));
    outParams.header.version      = 1;
    outParams.header.type         = CMD_LOAD_PARAMS;
    outParams.header.seq          = m_paramVersion++;
    outParams.paramVersion        = outParams.header.seq;

    /* Pack gain into payload. */
    ParamBlockHeader* hdr = reinterpret_cast<ParamBlockHeader*>(outParams.payload);
    hdr->version     = outParams.paramVersion;
    hdr->stageCount  = 1;

    StageParamEntry* entry = reinterpret_cast<StageParamEntry*>(
        outParams.payload + sizeof(ParamBlockHeader));
    entry->type      = STAGE_PARAM_GAIN;
    entry->stageSlot = 0;
    entry->dataLen   = sizeof(float);
    std::memcpy(entry->data, &gain, sizeof(float));

    outParams.payloadSize = sizeof(ParamBlockHeader) + sizeof(StageParamEntry);

    return true;
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
