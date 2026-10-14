#include "core/tcp_relay.h"
#include "core/logger.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/poll.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <chrono>
#include <thread>
#include <ctime>
#include <sstream>
#include <iomanip>
#include <iostream>

static bool waitForConnect(int sock, int timeoutMs) {
    for (int attempt = 0; attempt < timeoutMs / 50; ++attempt) {
        struct pollfd pfd;
        pfd.fd = sock;
        pfd.events = POLLOUT;
        pfd.revents = 0;
        int ret = poll(&pfd, 1, 50);
        if (ret > 0 && (pfd.revents & (POLLOUT | POLLHUP | POLLERR))) {
            int err = 0;
            socklen_t len = sizeof(err);
            if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &len) < 0) return false;
            return (err == 0);
        }
        if (ret < 0) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

TcpRelay::TcpRelay() {
    for (int i = 0; i < MAX_CHANNELS; i++) m_channelEnabled[i] = false;
}

TcpRelay::~TcpRelay() {
    disconnect();
}

bool TcpRelay::connect(const std::string& host, int port) {
    m_host = host;
    m_port = port;
    m_ctrlBufLen = 0;

    m_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (m_sock < 0) {
        Logger::getInstance().error("TcpRelay: socket() failed");
        return false;
    }

    int flags = fcntl(m_sock, F_GETFL, 0);
    fcntl(m_sock, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        Logger::getInstance().error("TcpRelay: invalid host " + host);
        close(m_sock);
        m_sock = -1;
        return false;
    }

    bool connected = false;
    int connRet = ::connect(m_sock, (struct sockaddr*)&addr, sizeof(addr));
    if (connRet == 0) {
        connected = true;
    } else if (connRet < 0 && errno == EINPROGRESS) {
        connected = waitForConnect(m_sock, 1000);
    }

    if (connected) {
        m_connected = true;
        Logger::getInstance().info("TcpRelay: connected to " + host + ":" + std::to_string(port));
    } else {
        Logger::getInstance().warning("TcpRelay: connect to " + host + ":" +
            std::to_string(port) + " failed — will retry in background");
        close(m_sock);
        m_sock = -1;
        m_connected = false;
    }

    m_running = true;
    m_sendThread = std::thread(&TcpRelay::sendLoop, this);
    m_ctrlThread = std::thread(&TcpRelay::ctrlLoop, this);
    return true;
}

void TcpRelay::pushChannel(uint8_t channelId, const float* mono, size_t frames) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_running) return;
        if (channelId >= MAX_CHANNELS || !m_channelEnabled[channelId]) return;
        auto& buf = m_buffers[channelId];
        buf.insert(buf.end(), mono, mono + frames);
    }
    m_dataCV.notify_one();
}

void TcpRelay::sendLoop() {
    while (m_running.load()) {
        if (!m_connected || m_sock < 0) {
            reconnect();
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            continue;
        }

        std::vector<std::pair<uint8_t, std::vector<float>>> batch;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_dataCV.wait_for(lock, std::chrono::milliseconds(5), [this] {
                for (auto& pair : m_buffers) {
                    if (!pair.second.empty()) return true;
                }
                return false;
            });
            for (auto& pair : m_buffers) {
                uint8_t ch = pair.first;
                if (ch >= MAX_CHANNELS || !m_channelEnabled[ch]) continue;
                auto& buf = pair.second;
                if (buf.empty()) continue;
                // Send everything available for this channel immediately
                std::vector<float> chunk(std::move(buf));
                batch.emplace_back(ch, std::move(chunk));
            }
            if (batch.empty()) continue;

            // Log batch composition
            static int batchLogCounter = 0;
            batchLogCounter++;
            if (batchLogCounter % 10 == 0) {
                std::string detail;
                for (auto& item : batch) {
                    detail += " ch" + std::to_string(item.first) + "=" + std::to_string(item.second.size()) + "f";
                }
                Logger::getInstance().info("[TCP-RELAY-BATCH] sent" + detail);
            }
        }

        for (auto& item : batch) {
            uint8_t channelId = item.first;
            auto& samples = item.second;
            size_t frames = samples.size();
            if (frames == 0) continue;

            size_t dataLen = frames * sizeof(float);
            size_t pktLen = 5 + dataLen;
            static thread_local std::vector<uint8_t> sendBuf;
            if (sendBuf.size() < pktLen) sendBuf.resize(pktLen);
            sendBuf[0] = channelId;
            uint32_t countBe = htonl(static_cast<uint32_t>(frames));
            std::memcpy(sendBuf.data() + 1, &countBe, 4);
            std::memcpy(sendBuf.data() + 5, samples.data(), dataLen);

            size_t totalSent = 0;
            bool fail = false;
            while (totalSent < pktLen) {
                ssize_t n = ::send(m_sock,
                    sendBuf.data() + totalSent,
                    pktLen - totalSent,
                    MSG_NOSIGNAL);
                if (n <= 0) {
                    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        continue;
                    }
                    m_connected = false;
                    fail = true;
                    break;
                }
                totalSent += n;
            }

            if (fail) {
                size_t sentSamples = (totalSent > 5 && totalSent < pktLen) ? (totalSent - 5) / sizeof(float) : 0;
                if (sentSamples < frames) {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    auto& chBuf = m_buffers[channelId];
                    chBuf.insert(chBuf.begin(), samples.data() + sentSamples, samples.data() + frames);
                }
                close(m_sock);
                m_sock = -1;
                reconnect();
                break;
            }
        }
    }
}

void TcpRelay::disconnect() {
    m_running = false;
    m_connected = false;
    m_dataCV.notify_all();
    if (m_ctrlThread.joinable()) m_ctrlThread.join();
    if (m_sendThread.joinable()) m_sendThread.join();
    if (m_sock >= 0) {
        close(m_sock);
        m_sock = -1;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_buffers.clear();
}

bool TcpRelay::isConnected() const {
    return m_connected && m_sock >= 0;
}

void TcpRelay::ctrlLoop() {
    while (m_running.load()) {
        if (m_sock < 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        char tmp[64];
        ssize_t n = ::recv(m_sock, tmp, sizeof(tmp), MSG_DONTWAIT | MSG_PEEK);
        if (n <= 0) continue;
        for (ssize_t i = 0; i < n; i++) {
            if (tmp[i] == '\n' && m_ctrlBufLen < static_cast<int>(sizeof(m_ctrlBuf) - 1)) {
                m_ctrlBuf[m_ctrlBufLen++] = tmp[i];
            } else if (tmp[i] != '\n') {
                if (m_ctrlBufLen < static_cast<int>(sizeof(m_ctrlBuf) - 1)) {
                    m_ctrlBuf[m_ctrlBufLen++] = tmp[i];
                }
            }
        }
        while (m_ctrlBufLen > 0) {
            char* nl = nullptr;
            bool found = false;
            for (int i = 0; i < m_ctrlBufLen; i++) {
                if (m_ctrlBuf[i] == '\n') { nl = &m_ctrlBuf[i]; found = true; break; }
            }
            if (!found) break;
            *nl = '\0';
            char cmd = m_ctrlBuf[0];
            int ch = -1;
            if (sscanf(m_ctrlBuf + 2, "%d", &ch) == 1 && ch >= 0 && ch < MAX_CHANNELS) {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_channelEnabled[ch] = (cmd == 'E');
            }
            int lineLen = static_cast<int>(nl - m_ctrlBuf) + 1;
            memmove(m_ctrlBuf, m_ctrlBuf + lineLen, m_ctrlBufLen - lineLen);
            m_ctrlBufLen -= lineLen;
        }
        if (n > 0) {
            ::recv(m_sock, tmp, n, MSG_DONTWAIT);
        }
    }
}

void TcpRelay::reconnect() {
    if (m_host.empty() || m_port == 0) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    m_ctrlBufLen = 0;
    for (int i = 0; i < MAX_CHANNELS; i++) m_channelEnabled[i] = false;

    m_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (m_sock < 0) return;
    int flags = fcntl(m_sock, F_GETFL, 0);
    fcntl(m_sock, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(m_port);
    if (inet_pton(AF_INET, m_host.c_str(), &addr.sin_addr) != 1) {
        close(m_sock);
        m_sock = -1;
        return;
    }

    int connRet = ::connect(m_sock, (struct sockaddr*)&addr, sizeof(addr));
    if (connRet == 0) {
        // connected
    } else if (connRet < 0 && errno == EINPROGRESS) {
        if (!waitForConnect(m_sock, 2000)) {
            close(m_sock);
            m_sock = -1;
            return;
        }
    } else {
        close(m_sock);
        m_sock = -1;
        return;
    }
    m_connected = true;
}
