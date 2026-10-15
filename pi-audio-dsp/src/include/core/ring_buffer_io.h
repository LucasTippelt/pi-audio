#ifndef PI_AUDIO_RING_BUFFER_IO_H
#define PI_AUDIO_RING_BUFFER_IO_H

#include <cstddef>
#include <cstdint>

bool createPgmShm(uint32_t sampleRate, int micCount);
void writePgmShm(const float* stereoData, size_t frames);
size_t readPgmShm(float* buffer, size_t maxFrames);
size_t readMicShm(int idx, float* buffer, size_t maxFrames);

#endif
