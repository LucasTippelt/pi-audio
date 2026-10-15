#ifndef PI_AUDIO_TEST_INJECT_H
#define PI_AUDIO_TEST_INJECT_H

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#pragma pack(push, 1)
struct TestInjectHeader {
    char    magic[4];     /* "PIIJ" */
    uint32_t frameCount;  /* total frames written */
    uint32_t readPos;     /* frames consumed */
};
#pragma pack(pop)

static const size_t kTestInjectHeaderSize = sizeof(TestInjectHeader);
static const size_t kTestInjectDataSize   = 4 * 1024 * 1024;

/* Reads up to maxFrames from a test injection shared memory buffer.
 * Advances readPos. Returns 0 when all frames consumed. */
inline size_t readTestInject(const std::string& shmName, float* out, size_t maxFrames) {
    int fd = shm_open(shmName.c_str(), O_RDWR, 0666);
    if (fd < 0) {
        Logger::getInstance().info("[TEST-OPEN] fail " + shmName + " errno=" + std::string(strerror(errno)));
        return 0;
    }

    void* ptr = mmap(nullptr, kTestInjectHeaderSize + kTestInjectDataSize,
                     PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) { close(fd); return 0; }

    TestInjectHeader* hdr = static_cast<TestInjectHeader*>(ptr);
    if (std::memcmp(hdr->magic, "PIIJ", 4) != 0) {
        munmap(ptr, kTestInjectHeaderSize + kTestInjectDataSize);
        close(fd);
        return 0;
    }

    uint32_t available = hdr->frameCount - hdr->readPos;
    if (available == 0) {
        munmap(ptr, kTestInjectHeaderSize + kTestInjectDataSize);
        close(fd);
        return 0;
    }
    static int hdrLogCnt = 0;
    if (hdrLogCnt++ < 5) {
        Logger::getInstance().info("[TEST-HEAD] " + shmName + " fc=" + std::to_string(hdr->frameCount) + " rp=" + std::to_string(hdr->readPos) + " av=" + std::to_string(available));
    }

    uint32_t toRead = (available < maxFrames) ? available : static_cast<uint32_t>(maxFrames);
    float* data = reinterpret_cast<float*>(static_cast<uint8_t*>(ptr) + kTestInjectHeaderSize);
    std::memcpy(out, data + hdr->readPos, toRead * sizeof(float));
    hdr->readPos += toRead;

    /* Reset when fully consumed */
    if (hdr->readPos >= hdr->frameCount) {
        hdr->readPos = 0;
        hdr->frameCount = 0;
    }

    munmap(ptr, kTestInjectHeaderSize + kTestInjectDataSize);
    close(fd);
    return toRead;
}

/* Writes mono float32 frames to a test injection shared memory buffer. */
inline int writeTestInject(const std::string& shmName, const float* data, size_t frames,
                           std::string* error = nullptr) {
    auto fail = [error](const char* operation) {
        int savedErrno = errno ? errno : EIO;
        if (error) *error = std::string(operation) + ": " + strerror(savedErrno);
        errno = savedErrno;
        return savedErrno;
    };
    if (frames > kTestInjectDataSize / sizeof(float)) {
        if (error) *error = "frame count exceeds shared-memory capacity";
        return EFBIG;
    }

    mode_t oldUmask = umask(0);
    int fd = shm_open(shmName.c_str(), O_RDWR, 0666);
    umask(oldUmask);
    if (fd < 0) return fail("shm_open");
    if (ftruncate(fd, kTestInjectHeaderSize + kTestInjectDataSize) < 0) {
        int result = fail("ftruncate");
        close(fd);
        return result;
    }

    void* ptr = mmap(nullptr, kTestInjectHeaderSize + kTestInjectDataSize,
                     PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) {
        int result = fail("mmap");
        close(fd);
        return result;
    }

    TestInjectHeader* hdr = static_cast<TestInjectHeader*>(ptr);
    std::memcpy(hdr->magic, "PIIJ", 4);
    hdr->frameCount = static_cast<uint32_t>(frames);
    hdr->readPos = 0;

    float* dest = reinterpret_cast<float*>(static_cast<uint8_t*>(ptr) + kTestInjectHeaderSize);
    std::memcpy(dest, data, frames * sizeof(float));

    if (msync(ptr, kTestInjectHeaderSize + kTestInjectDataSize, MS_SYNC) < 0) {
        int result = fail("msync");
        munmap(ptr, kTestInjectHeaderSize + kTestInjectDataSize);
        close(fd);
        return result;
    }
    munmap(ptr, kTestInjectHeaderSize + kTestInjectDataSize);
    close(fd);
    return 0;
}

/* Test mode control: writes/reads mode string to shared control file.
 * Mode is "tweeter", "woofer", "pgm-l", or empty (none). */

static const char* kTestModePath = "/dev/shm/pi-audio-test-mode";

inline void setTestMode(const std::string& mode) {
    int fd = open(kTestModePath, O_CREAT | O_WRONLY | O_TRUNC, 0666);
    if (fd < 0) return;
    const char* m = mode.empty() ? "none" : mode.c_str();
    write(fd, m, strlen(m));
    close(fd);
}

inline void clearTestMode() {
    unlink(kTestModePath);
}

inline std::string getTestMode() {
    char buf[32] = {0};
    int fd = open(kTestModePath, O_RDONLY, 0);
    if (fd < 0) return "";
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return "";
    return std::string(buf, n);
}

#endif
