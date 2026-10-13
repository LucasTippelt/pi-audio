#ifndef PI_AUDIO_TCP_RELAY_H
#define PI_AUDIO_TCP_RELAY_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>

class TcpRelay {
public:
    TcpRelay();
    ~TcpRelay();

    bool connect(const std::string& host, int port);

    /* Push stereo float32 data (non-blocking, called from playback thread). */
    void push(const float* stereo, size_t frames);

    void disconnect();
    bool isConnected() const;

private:
    int m_sock = -1;
    std::string m_host;
    int m_port = 0;
    std::atomic<bool> m_connected{false};
    std::atomic<bool> m_running{false};

    std::vector<float> m_buffer;
    std::mutex m_mutex;
    std::thread m_sendThread;

    void reconnect();
    void sendLoop();
};

#endif
