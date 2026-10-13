#ifndef PI_AUDIO_TCP_RELAY_H
#define PI_AUDIO_TCP_RELAY_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <map>

/* Channel IDs for per-channel mono monitoring */
enum MonitorChannelId : uint8_t {
    CH_PGM_L       = 0,
    CH_PGM_R       = 1,
    CH_WOOFER_L    = 2,
    CH_TWEETER_L   = 3,
    CH_WOOFER_R    = 4,
    CH_TWEETER_R   = 5,
    CH_MIC_1       = 6,
    CH_MIC_2       = 7,
    CH_MIC_3       = 8,
    CH_MIC_4       = 9,
};

class TcpRelay {
public:
    TcpRelay();
    ~TcpRelay();

    bool connect(const std::string& host, int port);

    /* Push mono float32 data for a specific channel (non-blocking, called from playback thread). */
    void pushChannel(uint8_t channelId, const float* mono, size_t frames);

    void disconnect();
    bool isConnected() const;

private:
    int m_sock = -1;
    std::string m_host;
    int m_port = 0;
    std::atomic<bool> m_connected{false};
    std::atomic<bool> m_running{false};

    /* Per-channel buffers: channelId -> mono float32 samples */
    std::map<uint8_t, std::vector<float>> m_buffers;
    std::mutex m_mutex;
    std::thread m_sendThread;

    void reconnect();
    void sendLoop();
};

#endif
