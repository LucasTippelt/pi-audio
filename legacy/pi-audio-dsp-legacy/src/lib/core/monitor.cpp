#include "core/monitor.h"
#include "core/logger.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <vector>

static const size_t kHeaderSize = sizeof(MonitorHeader);
static const size_t kDataSize = 4 * 1024 * 1024;
static const char* kRunDir = "/run/pi-audio";

static FILE* sFile = nullptr;
static std::string sPath;
static std::string sChannel;
static MonitorHeader sHeader;

std::string monitorChannelPath(const std::string& channel) {
    return std::string(kRunDir) + "/monitor-" + channel + ".raw";
}

void monitorInit(const std::string& channel, uint32_t sampleRate) {
    if (sFile) monitorClose();

    sChannel = channel;
    sPath = monitorChannelPath(channel);

    std::memset(&sHeader, 0, sizeof(sHeader));
    std::memcpy(sHeader.magic, "PIAM", 4);
    sHeader.version = 1;
    sHeader.writePos = 0;
    sHeader.sampleRate = sampleRate;
    sHeader.channels = 1;
    sHeader.seq = 0;
    if (!channel.empty()) {
        std::strncpy(sHeader.channelName, channel.c_str(), sizeof(sHeader.channelName) - 1);
    }

    std::remove(sPath.c_str()); /* remove stale file from another user */
    FILE* f = std::fopen(sPath.c_str(), "wb");
    if (!f) {
        Logger::getInstance().error("monitorInit: cannot open " + sPath);
        return;
    }

    std::fseek(f, 0, SEEK_SET);
    std::fwrite(&sHeader, kHeaderSize, 1, f);

    size_t toWrite = kDataSize;
    std::vector<char> zero(65536, 0);
    while (toWrite > 0) {
        size_t chunk = (zero.size() < toWrite) ? zero.size() : toWrite;
        std::fwrite(zero.data(), 1, chunk, f);
        toWrite -= chunk;
    }

    std::fflush(f);
    fchmod(fileno(f), 0666);
    std::fseek(f, 0, SEEK_SET);
    sFile = f;
    Logger::getInstance().info("Monitor ring buffer initialized: " + sPath +
                                " (channel=" + channel + ", " +
                                std::to_string(kDataSize) + " bytes, " +
                                std::to_string(sampleRate) + " Hz, mono)");
}

bool monitorOpenForWrite(const std::string& channel) {
    if (sFile) monitorClose();

    sChannel = channel;
    sPath = monitorChannelPath(channel);

    FILE* f = std::fopen(sPath.c_str(), "r+b");
    if (!f) {
        Logger::getInstance().warning("monitorOpenForWrite: cannot open " + sPath +
                                       " (ring buffer may not exist yet)");
        return false;
    }

    std::fseek(f, 0, SEEK_SET);
    std::vector<char> hdr(sizeof(MonitorHeader));
    if (std::fread(hdr.data(), 1, hdr.size(), f) != hdr.size()) {
        std::fclose(f);
        Logger::getInstance().error("monitorOpenForWrite: invalid header for " + channel);
        return false;
    }

    std::memcpy(&sHeader, hdr.data(), sizeof(MonitorHeader));

    if (std::memcmp(sHeader.magic, "PIAM", 4) != 0) {
        std::fclose(f);
        Logger::getInstance().error("monitorOpenForWrite: bad magic for " + channel);
        return false;
    }

    std::fseek(f, 0, SEEK_SET);
    sFile = f;
    return true;
}

void monitorWrite(const float* data, size_t frames) {
    if (!sFile || !data || frames == 0) return;

    /* Mono: 1 float per frame */
    size_t totalBytes = frames * sizeof(float);
    size_t pos = sHeader.writePos;

    std::fseek(sFile, static_cast<long>(kHeaderSize + pos), SEEK_SET);

    if (pos + totalBytes <= kDataSize) {
        std::fwrite(data, 1, totalBytes, sFile);
    } else {
        size_t first = kDataSize - pos;
        std::fwrite(data, 1, first, sFile);
        std::fseek(sFile, static_cast<long>(kHeaderSize), SEEK_SET);
        std::fwrite(reinterpret_cast<const char*>(data) + first, 1, totalBytes - first, sFile);
    }

    sHeader.writePos = (pos + totalBytes) % kDataSize;
    sHeader.seq++;

    std::fseek(sFile, 0, SEEK_SET);
    std::fwrite(&sHeader, kHeaderSize, 1, sFile);
    std::fflush(sFile);
}

void monitorClose() {
    if (sFile) {
        std::fclose(sFile);
        sFile = nullptr;
    }
}

bool monitorIsOpen() { return sFile != nullptr; }

const std::string& monitorPath() { return sPath; }

const std::string& monitorChannel() { return sChannel; }
