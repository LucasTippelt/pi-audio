#ifndef PI_AUDIO_WEBSOCKET_SERVER_H
#define PI_AUDIO_WEBSOCKET_SERVER_H

#include <cstdint>
#include <cstddef>
#include <utility>
#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <thread>
#include <functional>

/*
 * Minimal WebSocket server for real-time audio streaming.
 *
 * Embeds in the C++ engine on port 8768. Provides application-level framing
 * so each 512-frame buffer arrives as a distinct binary frame, bypassing
 * browser TCP receive batching.
 *
 * Protocol:
 *   Subscribe: Client sends text frame {"channel":"tweeter"}
 *   Ack: Server replies {"ok":true,"sampleRate":48000,"bufferSize":512}
 *   Audio: Server pushes binary frames — raw mono float32LE, 512 frames per frame
 *   Unsubscribe: Client closes connection
 */

class WebSocketServer {
public:
    WebSocketServer();
    ~WebSocketServer();

    bool start(const std::string& host, int port);
    void stop();

    void pushAudio(uint8_t channelId, const float* data, size_t frames);

    void setSampleRate(int sr) { m_sampleRate = sr; }
    void setBufferSize(int bs) { m_bufferSize = bs; }

    /* Set a callback that returns the current {sampleRate, bufferSize}.
     * Takes priority over setSampleRate/setBufferSize when set. */
    void setConfigCallback(std::function<std::pair<int, int>()> cb) { m_configCb = cb; }

    bool isRunning() const { return m_running.load(); }

private:
    void acceptLoop();
    void clientLoop(int sock);

    static std::vector<uint8_t> sha1(const std::vector<uint8_t>& data);
    static std::string base64(const std::vector<uint8_t>& data);
    static void unmask(uint8_t* payload, size_t len, const uint8_t* mask);

    bool doHandshake(int sock, std::string& request);
    bool readFrame(int sock, uint8_t* payload, size_t& payloadLen, bool& fin, uint8_t& opcode);
    void sendFrame(int sock, const void* data, size_t len, uint8_t opcode);
    void sendText(int sock, const std::string& text);
    void sendBinary(int sock, const void* data, size_t len);

    struct Client {
        int sock = -1;
        uint8_t channel = 0;
        bool subscribed = false;
        /* Debug stats */
        uint64_t framesSent = 0;
        uint64_t framesDropped = 0;
        uint64_t bytesSent = 0;
        double lastPushMs = 0;
    };

    std::atomic<bool> m_running{false};
    int m_serverSock = -1;
    std::thread m_acceptThread;

    std::mutex m_clientsMutex;
    std::vector<Client*> m_clients;

    int m_sampleRate = 48000;
    int m_bufferSize = 512;
    std::function<std::pair<int, int>()> m_configCb;
};

#endif
