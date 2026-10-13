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
#include <ctime>
#include <sstream>
#include <iomanip>
#include <iostream>

TcpRelay::TcpRelay() {
    for (int i = 0; i < MAX_CHANNELS; i++) m_channelEnabled[i] = false;
}

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
    m_ctrlBufLen = 0;
    m_sendThread = std::thread(&TcpRelay::sendLoop, this);
    m_ctrlThread = std::thread(&TcpRelay::ctrlLoop, this);
    Logger::getInstance().info("TcpRelay: connected to " + host + ":" + std::to_string(port));
    return true;
}

/* Called from playback thread: append mono data for a channel, never block. */
void TcpRelay::pushChannel(uint8_t channelId, const float* mono, size_t frames) {
    /* Push rate logger: log every 5 seconds */
    static std::chrono::steady_clock::time_point pushRateStart = std::chrono::steady_clock::now();
    static uint64_t pushTotalSamples = 0;
    static uint64_t pushTotalFrames = 0;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_running) return;
        if (channelId >= MAX_CHANNELS || !m_channelEnabled[channelId]) return;
        auto& buf = m_buffers[channelId];
        buf.insert(buf.end(), mono, mono + frames);
    }
    m_dataCV.notify_one();

    pushTotalSamples += frames;
    pushTotalFrames++;
    auto now = std::chrono::steady_clock::now();
    double dt = std::chrono::duration<double>(now - pushRateStart).count();
    if (dt >= 5.0) {
        pushRateStart = now;
        double rate = pushTotalSamples / dt;
        std::ostringstream oss;
        oss << "[PUSH] " << pushTotalSamples << " samples/"
            << std::fixed << std::setprecision(1) << dt << "s = "
            << std::fixed << std::setprecision(0) << rate << " samples/s ("
            << pushTotalFrames << " callbacks, avg=" << (pushTotalSamples / pushTotalFrames) << " frames/cb)";
        std::cerr << oss.str() << std::endl;
        pushTotalSamples = 0;
        pushTotalFrames = 0;
    }
}

void TcpRelay::sendLoop() {
    /* Rate logger: log every 5 seconds */
    auto rateStartTime = std::chrono::steady_clock::now();
    uint64_t rateTotalSamples = 0;
    uint64_t rateTotalMsgs = 0;
    double maxSendMs = 0;

    while (m_running.load()) {
        std::map<uint8_t, std::vector<float>> batch;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            if (!m_connected || m_sock < 0) {
                lock.unlock();
                reconnect();
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }
            m_dataCV.wait_for(lock, std::chrono::milliseconds(1), [this] {
                for (auto& pair : m_buffers) {
                    if (!pair.second.empty()) return true;
                }
                return false;
            });
            bool anyData = false;
            for (auto& pair : m_buffers) {
                if (!pair.second.empty() && pair.first < MAX_CHANNELS && m_channelEnabled[pair.first]) {
                    batch[pair.first].swap(pair.second);
                    anyData = true;
                }
            }
            if (!anyData) continue;
        }

        for (auto& pair : batch) {
            uint8_t channelId = pair.first;
            if (channelId >= MAX_CHANNELS) continue;
            bool enabled = false;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                enabled = m_channelEnabled[channelId];
            }
            if (!enabled) continue;
            auto& samples = pair.second;
            size_t frames = samples.size();
            if (frames == 0) continue;

            /* Build single packet: [1 byte channel ID][4 bytes BE sample count][frames × 4 bytes mono float32] */
            size_t dataLen = frames * sizeof(float);
            size_t pktLen = 5 + dataLen;
            /* Reuse or allocate send buffer */
            static thread_local std::vector<uint8_t> sendBuf;
            if (sendBuf.size() < pktLen) sendBuf.resize(pktLen);
            sendBuf[0] = channelId;
            uint32_t countBe = htonl(static_cast<uint32_t>(frames));
            std::memcpy(sendBuf.data() + 1, &countBe, 4);
            std::memcpy(sendBuf.data() + 5, samples.data(), dataLen);

            /* Send entire packet in one call — measure send time */
            auto sendStart = std::chrono::high_resolution_clock::now();
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
                /* Put unsent samples back */
                size_t sentSamples = (totalSent > 5 && totalSent < pktLen) ? (totalSent - 5) / sizeof(float) : 0;
                if (sentSamples < frames) {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    auto& chBuf = m_buffers[channelId];
                    chBuf.insert(chBuf.begin(), samples.data() + sentSamples, samples.data() + frames);
                }
                Logger::getInstance().warning("TcpRelay: data send fail for ch=" + std::to_string(channelId) + ", reconnect");
                close(m_sock);
                m_sock = -1;
                reconnect();
                break;
            }

            if (!fail) {
                rateTotalSamples += frames;
                rateTotalMsgs++;
                auto sendEnd = std::chrono::high_resolution_clock::now();
                double sendMs = std::chrono::duration<double, std::milli>(sendEnd - sendStart).count();
                if (sendMs > maxSendMs) maxSendMs = sendMs;
                if (sendMs > 2.0) {
                    std::ostringstream oss;
                    oss << "[RELAY-SLOW] send=" << std::fixed << std::setprecision(1) << sendMs << "ms pkt=" << pktLen << " max=" << std::setprecision(1) << maxSendMs << "ms";
                    std::cerr << oss.str() << std::endl;
                }
            }

            if (fail) {
                auto sendEnd = std::chrono::high_resolution_clock::now();
                double sendMs = std::chrono::duration<double, std::milli>(sendEnd - sendStart).count();
                std::ostringstream oss;
                oss << "[RELAY-FAIL] send=" << std::fixed << std::setprecision(1) << sendMs << "ms totalSent=" << totalSent << "/" << pktLen;
                std::cerr << oss.str() << std::endl;
            }

            /* Periodic rate log every 5 seconds to stderr (-> journal) */
            auto now = std::chrono::steady_clock::now();
            double dt = std::chrono::duration<double>(now - rateStartTime).count();
            if (dt >= 5.0) {
                rateStartTime = now;
                double rate = rateTotalSamples / dt;
                std::ostringstream oss;
                oss << "[RELAY] send=" << rateTotalSamples << " samples/"
                    << std::fixed << std::setprecision(1) << dt << "s = "
                    << std::fixed << std::setprecision(0) << rate << " samples/s ("
                    << rateTotalMsgs << " msgs) maxSend="
                    << std::fixed << std::setprecision(1) << maxSendMs << "ms";
                std::cerr << oss.str() << std::endl;
                rateTotalSamples = 0;
                rateTotalMsgs = 0;
            }

            if (fail) {
                Logger::getInstance().warning("TcpRelay: data send fail for ch=" + std::to_string(channelId) + ", reconnect");
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
    if (m_ctrlThread.joinable()) {
        m_ctrlThread.join();
    }
    if (m_sendThread.joinable()) {
        m_sendThread.join();
    }
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
    while (m_running.load() && m_sock >= 0) {
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
    if (::connect(m_sock, (struct sockaddr*)&addr, sizeof(addr)) < 0 && errno != EINPROGRESS) {
        close(m_sock);
        m_sock = -1;
        return;
    }
    m_connected = true;
    Logger::getInstance().info("TcpRelay: connected to " + m_host + ":" + std::to_string(m_port));
}
