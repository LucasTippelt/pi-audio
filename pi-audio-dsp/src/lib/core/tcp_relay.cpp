#include "core/tcp_relay.h"
#include "core/logger.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>
#include <chrono>
#include <thread>

TcpRelay::TcpRelay() {}

TcpRelay::~TcpRelay() {
    disconnect();
}

bool TcpRelay::connect(const std::string& host, int port) {
    m_host = host;
    m_port = port;

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

    if (::connect(m_sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        if (errno != EINPROGRESS) {
            Logger::getInstance().error("TcpRelay: connect failed " + host + ":" + std::to_string(port));
            close(m_sock);
            m_sock = -1;
            return false;
        }
    }

    m_connected = true;
    m_running = true;
    m_sendThread = std::thread(&TcpRelay::sendLoop, this);
    Logger::getInstance().info("TcpRelay: connected to " + host + ":" + std::to_string(port));
    return true;
}

/* Called from playback thread: just append to buffer, never block. */
void TcpRelay::push(const float* stereo, size_t frames) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_running) return;
    m_buffer.insert(m_buffer.end(), stereo, stereo + frames * 2);
}

void TcpRelay::sendLoop() {
    while (m_running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));

        std::vector<float> batch;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_buffer.empty() || !m_connected || m_sock < 0) continue;
            batch.swap(m_buffer);
        }

        size_t frames = batch.size() / 2;
        if (frames == 0) continue;

        uint32_t countBe = htonl(static_cast<uint32_t>(frames));
        ssize_t sent = ::send(m_sock, &countBe, 4, MSG_NOSIGNAL);
        if (sent != 4) {
            if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
            m_connected = false;
            Logger::getInstance().warning("TcpRelay: header send fail, reconnect");
            close(m_sock);
            m_sock = -1;
            reconnect();
            continue;
        }

        size_t dataLen = batch.size() * sizeof(float);
        size_t totalSent = 0;
        bool fail = false;
        while (totalSent < dataLen) {
            ssize_t n = ::send(m_sock,
                (const char*)batch.data() + totalSent,
                dataLen - totalSent,
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
            Logger::getInstance().warning("TcpRelay: data send fail, reconnect");
            close(m_sock);
            m_sock = -1;
            reconnect();
        }
    }
}

void TcpRelay::disconnect() {
    m_running = false;
    m_connected = false;
    if (m_sendThread.joinable()) {
        m_sendThread.join();
    }
    if (m_sock >= 0) {
        close(m_sock);
        m_sock = -1;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_buffer.clear();
}

bool TcpRelay::isConnected() const {
    return m_connected && m_sock >= 0;
}

void TcpRelay::reconnect() {
    if (m_host.empty() || m_port == 0) return;
    Logger::getInstance().info("TcpRelay: reconnecting...");
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

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
    if (::connect(m_sock, (struct sockaddr*)&addr, sizeof(addr)) < 0 && errno != EINPROGRESS) {
        close(m_sock);
        m_sock = -1;
        return;
    }
    m_connected = true;
}
