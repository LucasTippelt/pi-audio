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
#include <condition_variable>

/* Channel IDs for single speaker system */
enum MonitorChannelId : uint8_t {
    CH_PGM_L       = 0,
    CH_PGM_R       = 1,
    CH_TWEETER     = 2,
    CH_WOOFER      = 3,
    CH_MIC_BASE    = 4,
};

static const int MAX_CHANNELS = 10;

class TcpRelay {
public:
    TcpRelay();
    ~TcpRelay();

    bool connect(const std::string& host, int port);
    void pushChannel(uint8_t channelId, const float* mono, size_t frames);
    void disconnect();
    bool isConnected() const;

private:
    int m_sock = -1;
    std::string m_host;
    int m_port = 0;
    std::atomic<bool> m_connected{false};
    std::atomic<bool> m_running{false};

    std::map<uint8_t, std::vector<float>> m_buffers;
    std::mutex m_mutex;
    std::condition_variable m_dataCV;
    std::thread m_sendThread;

    bool m_channelEnabled[MAX_CHANNELS];
    std::mutex m_ctrlMutex;
    std::thread m_ctrlThread;
    char m_ctrlBuf[256];
    int m_ctrlBufLen = 0;

    void ctrlLoop();
    void reconnect();
    void sendLoop();
};

inline TcpRelay& tcpRelay() {
    static TcpRelay inst;
    return inst;
}

#endif
