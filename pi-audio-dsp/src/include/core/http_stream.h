#ifndef PI_AUDIO_HTTP_STREAM_H
#define PI_AUDIO_HTTP_STREAM_H

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <thread>
#include <functional>

/*
 * Minimal HTTP streaming server for real-time audio monitoring.
 *
 * Listens on a TCP port. Accepts connections and serves:
 *   GET /stream?ch=<channel>  — continuous raw float32 mono audio stream
 *   GET /status               — JSON engine status
 *   GET /                     — JSON channel list
 *
 * The /stream endpoint uses HTTP chunked transfer encoding.
 * Audio data is pushed by the engine via pushSamples().
 * Each connected client receives a copy of the pushed data for its channel.
 */

class HttpStreamServer {
public:
    HttpStreamServer();
    ~HttpStreamServer();

    /* Start listening on the given port. Returns true on success. */
    bool start(const std::string& host, int port);

    /* Stop the server and close all connections. */
    void stop();

    /* Push mono float32 samples for a channel (0-9). All connected clients for that channel receive the data. */
    void pushSamples(uint8_t channelId, const float* data, size_t frames);

    /* Set a callback that returns JSON status text. Called on each /status request. */
    void setStatusCallback(std::function<std::string()> cb);

    /* Whether the server is currently running. */
    bool isRunning() const { return m_running.load(); }

    void setBufferSize(int bs) { m_bufferSize = bs; }

private:
    void acceptLoop();
    void clientLoop(int sock);

    struct Client {
        int sock = -1;
        uint8_t channel = 0;
        bool streaming = false;
        std::string request;
        /* Monitoring stats */
        std::atomic<uint64_t> bytesSent{0};
        std::atomic<uint64_t> chunksSent{0};
        std::atomic<uint64_t> chunksDropped{0};
        std::atomic<uint64_t> eagainCount{0};
        /* Debug timing */
        double lastPushMs = 0;
        uint64_t lastPushSeq = 0;
    };

    std::atomic<bool> m_running{false};
    int m_serverSock = -1;
    std::thread m_acceptThread;

    std::mutex m_clientsMutex;
    std::vector<Client*> m_clients;

    std::function<std::string()> m_statusCb;

    int m_bufferSize = 512;
};

#endif
