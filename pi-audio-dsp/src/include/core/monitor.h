#ifndef PI_AUDIO_MONITOR_H
#define PI_AUDIO_MONITOR_H

#include <cstdint>
#include <cstddef>
#include <string>

/*
 * Per-channel audio monitor ring buffers for debug playback.
 * Each channel gets its own mono ring buffer file:
 *   /run/pi-audio/monitor-{channel}.raw
 *
 * Supported channels: pgm-l, pgm-r, woofer-l, tweeter-l, woofer-r, tweeter-r, mic
 *
 * File layout:
 *   [64-byte header][raw float32 mono data...]
 *
 * Data size: 4 MB per channel (~8.3s of 48kHz mono float32)
 *
 * Header (64 bytes, packed):
 *   magic[4]      "PIAM"
 *   version[2]    1
 *   writePos[8]   byte offset of next write (0..kDataSize-1)
 *   sampleRate[4] Hz
 *   channels[2]   always 1 (mono per channel)
 *   seq[4]        monotonically increasing write sequence counter
 *   channelName[16] null-terminated channel identifier
 *   reserved[20]
 */

#pragma pack(push, 1)
struct MonitorHeader {
    char magic[4];
    uint16_t version;
    uint64_t writePos;
    uint32_t sampleRate;
    uint16_t channels;
    uint32_t seq;
    char channelName[16];
    char reserved[20];
};
#pragma pack(pop)

/* Initialize a new ring buffer for the given channel. */
void monitorInit(const std::string& channel, uint32_t sampleRate);

/* Open an existing ring buffer for the given channel for writing. */
bool monitorOpenForWrite(const std::string& channel);

/* Write mono float32 samples to the currently open channel buffer. */
void monitorWrite(const float* data, size_t frames);

/* Close the currently open channel buffer. */
void monitorClose();

/* Check if a channel buffer is currently open. */
bool monitorIsOpen();

/* Get the path of the currently open channel buffer. */
const std::string& monitorPath();

/* Get the channel name of the currently open buffer. */
const std::string& monitorChannel();

/* Build the file path for a given channel. */
std::string monitorChannelPath(const std::string& channel);

#endif
