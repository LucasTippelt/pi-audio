#ifndef PI_AUDIO_TEST_INJECT_H
#define PI_AUDIO_TEST_INJECT_H

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
    if (fd < 0) return 0;

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
inline int writeTestInject(const std::string& shmName, const float* data, size_t frames) {
    int fd = shm_open(shmName.c_str(), O_CREAT | O_RDWR, 0666);
    if (fd < 0) return 1;
    ftruncate(fd, kTestInjectHeaderSize + kTestInjectDataSize);

    void* ptr = mmap(nullptr, kTestInjectHeaderSize + kTestInjectDataSize,
                     PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) { close(fd); return 1; }

    TestInjectHeader* hdr = static_cast<TestInjectHeader*>(ptr);
    std::memcpy(hdr->magic, "PIIJ", 4);
    hdr->frameCount = static_cast<uint32_t>(frames);
    hdr->readPos = 0;

    float* dest = reinterpret_cast<float*>(static_cast<uint8_t*>(ptr) + kTestInjectHeaderSize);
    std::memcpy(dest, data, frames * sizeof(float));

    munmap(ptr, kTestInjectHeaderSize + kTestInjectDataSize);
    close(fd);
    return 0;
}

#endif
