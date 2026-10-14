#include "core/websocket_server.h"
#include "core/logger.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <chrono>
#include <thread>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <vector>
#include <cstdint>

WebSocketServer::WebSocketServer() {}

WebSocketServer::~WebSocketServer() {
    stop();
}

/* ------------------------------------------------------------------ */
/*  SHA-1 (RFC 3174) — minimal implementation for Sec-WebSocket-Accept */
/* ------------------------------------------------------------------ */

static uint32_t sha1_rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

std::vector<uint8_t> WebSocketServer::sha1(const std::vector<uint8_t>& input) {
    static const uint32_t K[4] = {0x5A827999, 0x6ED9EBA1, 0x8F1BBCDC, 0xCA62C1D6};

    uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE, h3 = 0x10325476, h4 = 0xC3D2E1F0;

    size_t msgLen = input.size();
    uint64_t bitLen = msgLen * 8;

    std::vector<uint8_t> msg(input.begin(), input.end());
    msg.push_back(0x80);
    while (msg.size() % 64 != 56) msg.push_back(0);
    for (int i = 7; i >= 0; --i) msg.push_back(static_cast<uint8_t>((bitLen >> (i * 8)) & 0xFF));

    for (size_t offset = 0; offset < msg.size(); offset += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (uint32_t)msg[offset + i * 4] << 24 |
                   (uint32_t)msg[offset + i * 4 + 1] << 16 |
                   (uint32_t)msg[offset + i * 4 + 2] << 8 |
                   (uint32_t)msg[offset + i * 4 + 3];
        }
        for (int i = 16; i < 80; ++i)
            w[i] = sha1_rotr(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);

        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;

        for (int i = 0; i < 80; ++i) {
            uint32_t f, temp;
            if (i < 20)        { f = (b & c) | (~b & d);           temp = K[0]; }
            else if (i < 40)   { f = b ^ c ^ d;                    temp = K[1]; }
            else if (i < 60)   { f = (b & c) | (b & d) | (c & d);  temp = K[2]; }
            else               { f = b ^ c ^ d;                    temp = K[3]; }
            temp = sha1_rotr(a, 5) + f + e + temp + w[i];
            e = d; d = c; c = sha1_rotr(b, 30); b = a; a = temp;
        }

        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }

    std::vector<uint8_t> digest(20);
    for (int i = 0; i < 4; ++i) {
        digest[i * 5 + 0] = (h0 >> 24) & 0xFF;
        digest[i * 5 + 1] = (h0 >> 16) & 0xFF;
        digest[i * 5 + 2] = (h0 >> 8) & 0xFF;
        digest[i * 5 + 3] = h0 & 0xFF;
    }
    for (int i = 0; i < 4; ++i) {
        digest[i * 5 + 0] = (h1 >> 24) & 0xFF;
        digest[i * 5 + 1] = (h1 >> 16) & 0xFF;
        digest[i * 5 + 2] = (h1 >> 8) & 0xFF;
        digest[i * 5 + 3] = h1 & 0xFF;
    }
    for (int i = 0; i < 4; ++i) {
        digest[i * 5 + 0] = (h2 >> 24) & 0xFF;
        digest[i * 5 + 1] = (h2 >> 16) & 0xFF;
        digest[i * 5 + 2] = (h2 >> 8) & 0xFF;
        digest[i * 5 + 3] = h2 & 0xFF;
    }
    for (int i = 0; i < 4; ++i) {
        digest[i * 5 + 0] = (h3 >> 24) & 0xFF;
        digest[i * 5 + 1] = (h3 >> 16) & 0xFF;
        digest[i * 5 + 2] = (h3 >> 8) & 0xFF;
        digest[i * 5 + 3] = h3 & 0xFF;
    }
    for (int i = 0; i < 4; ++i) {
        digest[i * 5 + 0] = (h4 >> 24) & 0xFF;
        digest[i * 5 + 1] = (h4 >> 16) & 0xFF;
        digest[i * 5 + 2] = (h4 >> 8) & 0xFF;
        digest[i * 5 + 3] = h4 & 0xFF;
    }
    return digest;
}

std::string WebSocketServer::base64(const std::vector<uint8_t>& data) {
    static const char chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve((data.size() + 2) / 3 * 4);
    for (size_t i = 0; i < data.size(); i += 3) {
        uint32_t n = data[i] << 16;
        if (i + 1 < data.size()) n |= data[i + 1] << 8;
        if (i + 2 < data.size()) n |= data[i + 2];
        result += chars[(n >> 18) & 0x3F];
        result += chars[(n >> 12) & 0x3F];
        result += (i + 1 < data.size()) ? chars[(n >> 6) & 0x3F] : '=';
        result += (i + 2 < data.size()) ? chars[n & 0x3F] : '=';
    }
    return result;
}

void WebSocketServer::unmask(uint8_t* payload, size_t len, const uint8_t* mask) {
    for (size_t i = 0; i < len; ++i) payload[i] ^= mask[i % 4];
}

/* ------------------------------------------------------------------ */
/*  Server lifecycle                                                    */
/* ------------------------------------------------------------------ */

bool WebSocketServer::start(const std::string& host, int port) {
    m_serverSock = socket(AF_INET, SOCK_STREAM, 0);
    if (m_serverSock < 0) {
        Logger::getInstance().error("WebSocketServer: socket() failed: " + std::string(strerror(errno)));
        return false;
    }

    int opt = 1;
    setsockopt(m_serverSock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(m_serverSock, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (host.empty() || host == "0.0.0.0") {
        addr.sin_addr.s_addr = INADDR_ANY;
        Logger::getInstance().info("WebSocketServer: binding to 0.0.0.0:" + std::to_string(port));
    } else {
        inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
        Logger::getInstance().info("WebSocketServer: binding to " + host + ":" + std::to_string(port));
    }

    if (::bind(m_serverSock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        Logger::getInstance().error("WebSocketServer: bind failed on port " +
            std::to_string(port) + " — errno=" + std::to_string(errno) + " (" + strerror(errno) + ")");
        close(m_serverSock);
        m_serverSock = -1;
        return false;
    }

    if (::listen(m_serverSock, 16) < 0) {
        Logger::getInstance().error("WebSocketServer: listen failed: " + std::string(strerror(errno)));
        close(m_serverSock);
        m_serverSock = -1;
        return false;
    }

    m_running = true;
    m_acceptThread = std::thread(&WebSocketServer::acceptLoop, this);
    Logger::getInstance().info("WebSocketServer: listening on port " + std::to_string(port));
    return true;
}

void WebSocketServer::stop() {
    Logger::getInstance().info("WebSocketServer: stopping (" +
        std::to_string(m_clients.size()) + " clients)");
    m_running = false;
    if (m_serverSock >= 0) {
        close(m_serverSock);
        m_serverSock = -1;
    }
    if (m_acceptThread.joinable()) m_acceptThread.join();

    std::lock_guard<std::mutex> lock(m_clientsMutex);
    for (auto* c : m_clients) {
        if (c->sock >= 0) shutdown(c->sock, SHUT_RDWR);
    }
    Logger::getInstance().info("WebSocketServer: stopped");
}

/* ------------------------------------------------------------------ */
/*  Push audio from playback thread                                     */
/* ------------------------------------------------------------------ */

void WebSocketServer::pushAudio(uint8_t channelId, const float* data, size_t frames) {
    if (!m_running.load() || !data || frames == 0) return;

    size_t dataLen = frames * sizeof(float);
    auto tNow = std::chrono::steady_clock::now();
    double nowMs = std::chrono::duration<double, std::milli>(tNow.time_since_epoch()).count();

    /* Build frame header inline: FIN+binary(0x82) + length */
    uint8_t hdr[3];
    hdr[0] = 0x82;
    if (dataLen <= 125) {
        hdr[1] = static_cast<uint8_t>(dataLen);
    } else {
        hdr[1] = 126;
        hdr[2] = static_cast<uint8_t>((dataLen >> 8) & 0xFF);
    }
    size_t hdrLen = (dataLen <= 125) ? 2 : 3;

    std::lock_guard<std::mutex> lock(m_clientsMutex);
    auto tLock = std::chrono::steady_clock::now();
    double lockUs = std::chrono::duration<double, std::micro>(tLock - tNow).count();
    std::vector<Client*> dead;
    int delivered = 0, dropped = 0;

    for (auto* c : m_clients) {
        if (!c->subscribed || c->channel != channelId || c->sock < 0) continue;

        /* Send header + payload as two sends. With small SO_SNDBUF and
           TCP_NODELAY, each pair should flush as one TCP packet. */
        ssize_t n = ::send(c->sock, reinterpret_cast<const char*>(hdr), hdrLen, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n > 0 && n == static_cast<ssize_t>(hdrLen)) {
            n = ::send(c->sock,
                reinterpret_cast<const char*>(data),
                dataLen,
                MSG_NOSIGNAL | MSG_DONTWAIT);
            if (n > 0 && n == static_cast<ssize_t>(dataLen)) {
                c->framesSent++;
                c->bytesSent += dataLen;
                c->lastPushMs = nowMs;
                delivered++;
            } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                c->framesDropped++;
                dropped++;
            } else {
                dead.push_back(c);
            }
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            c->framesDropped++;
            dropped++;
        } else {
            dead.push_back(c);
        }
    }

    auto tEnd = std::chrono::steady_clock::now();
    double totalUs = std::chrono::duration<double, std::micro>(tEnd - tNow).count();

    if (dropped > 0) {
        Logger::getInstance().warning(
            "WS push ch=" + std::to_string(channelId) +
            " frames=" + std::to_string(frames) +
            " delivered=" + std::to_string(delivered) +
            " dropped=" + std::to_string(dropped) +
            " lock=" + std::to_string((int)lockUs) + "us" +
            " total=" + std::to_string((int)totalUs) + "us");
    }

    for (auto* c : dead) {
        if (c->sock >= 0) close(c->sock);
        c->sock = -1;
        c->subscribed = false;
        delete c;
    }
}

/* ------------------------------------------------------------------ */
/*  Accept loop                                                         */
/* ------------------------------------------------------------------ */

void WebSocketServer::acceptLoop() {
    while (m_running.load()) {
        struct sockaddr_in addr;
        socklen_t addrLen = sizeof(addr);
        int clientSock = ::accept(m_serverSock, (struct sockaddr*)&addr, &addrLen);
        if (clientSock < 0) {
            if (m_running.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            continue;
        }

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
        Logger::getInstance().info("WebSocketServer: connection from " +
            std::string(ip) + ":" + std::to_string(ntohs(addr.sin_port)));

        int tcpOpt = 1;
        setsockopt(clientSock, IPPROTO_TCP, TCP_NODELAY, &tcpOpt, sizeof(tcpOpt));
        /* Small send buffer: one frame (2052B) + margin. Forces each frame
           to flush as its own TCP packet instead of accumulating in the
           kernel buffer and arriving at the browser in bursts. */
        int bufSize = 4096;
        setsockopt(clientSock, SOL_SOCKET, SO_SNDBUF, &bufSize, sizeof(bufSize));

        Client* c = new Client();
        c->sock = clientSock;
        {
            std::lock_guard<std::mutex> lock(m_clientsMutex);
            m_clients.push_back(c);
        }
        std::thread([this, c]() { clientLoop(c->sock); }).detach();
    }
}

/* ------------------------------------------------------------------ */
/*  Handshake                                                           */
/* ------------------------------------------------------------------ */

bool WebSocketServer::doHandshake(int sock, std::string& request) {
    size_t keyPos = request.find("Sec-WebSocket-Key: ");
    if (keyPos == std::string::npos) {
        Logger::getInstance().warning("WebSocketServer: no Sec-WebSocket-Key");
        return false;
    }
    std::string key = request.substr(keyPos + 21);
    size_t eol = key.find('\r');
    if (eol != std::string::npos) key = key.substr(0, eol);
    eol = key.find('\n');
    if (eol != std::string::npos) key = key.substr(0, eol);

    std::string magic = "258EAFA5-E914-47DA-95CA-5AB5DC7E9B16";
    std::vector<uint8_t> concat(key.begin(), key.end());
    concat.insert(concat.end(), magic.begin(), magic.end());

    std::vector<uint8_t> hash = sha1(concat);
    std::string accept = base64(hash);

    std::string response =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " + accept + "\r\n"
        "Access-Control-Allow-Origin: *\r\n\r\n";

    ssize_t sent = ::send(sock, response.data(), response.size(), MSG_NOSIGNAL);
    if (sent <= 0) {
        Logger::getInstance().warning("WebSocketServer: handshake send failed");
        return false;
    }
    Logger::getInstance().info("WebSocketServer: handshake complete");
    return true;
}

/* ------------------------------------------------------------------ */
/*  Frame read (client → server, masked)                                */
/* ------------------------------------------------------------------ */

bool WebSocketServer::readFrame(int sock, uint8_t* payload, size_t& payloadLen, bool& fin, uint8_t& opcode) {
    uint8_t header[2];
    ssize_t n = ::recv(sock, header, 2, MSG_WAITALL);
    if (n <= 0) return false;

    fin = (header[0] & 0x80) != 0;
    opcode = header[0] & 0x0F;
    bool maskBit = (header[1] & 0x80) != 0;
    uint64_t len = header[1] & 0x7F;

    if (len == 126) {
        uint8_t ext[2];
        n = ::recv(sock, ext, 2, MSG_WAITALL);
        if (n <= 0) return false;
        len = (uint64_t(ext[0]) << 8) | ext[1];
    } else if (len == 127) {
        uint8_t ext[8];
        n = ::recv(sock, ext, 8, MSG_WAITALL);
        if (n <= 0) return false;
        len = 0;
        for (int i = 0; i < 8; ++i) len = (len << 8) | ext[i];
    }

    if (len > 65536) {
        Logger::getInstance().warning("WebSocketServer: frame too large (" + std::to_string(len) + ")");
        return false;
    }

    if (maskBit) {
        uint8_t mask[4];
        n = ::recv(sock, mask, 4, MSG_WAITALL);
        if (n <= 0) return false;
        std::vector<uint8_t> buf(len);
        n = ::recv(sock, buf.data(), len, MSG_WAITALL);
        if (n <= 0) return false;
        unmask(buf.data(), len, mask);
        if (payload) std::memcpy(payload, buf.data(), len);
    } else {
        n = ::recv(sock, payload, len, MSG_WAITALL);
        if (n <= 0) return false;
    }

    payloadLen = len;
    return true;
}

/* ------------------------------------------------------------------ */
/*  Frame write (server → client, unmasked)                             */
/* ------------------------------------------------------------------ */

void WebSocketServer::sendFrame(int sock, const void* data, size_t len, uint8_t opcode) {
    std::vector<uint8_t> frame;
    size_t headerLen = (len <= 125) ? 2 : (len <= 65535) ? 3 : 10;
    frame.resize(headerLen + len);
    frame[0] = 0x80 | opcode;

    if (len <= 125) {
        frame[1] = static_cast<uint8_t>(len);
    } else if (len <= 65535) {
        frame[1] = 126;
        frame[2] = static_cast<uint8_t>((len >> 8) & 0xFF);
        frame[3] = static_cast<uint8_t>(len & 0xFF);
    } else {
        frame[1] = 127;
        for (int i = 7; i >= 0; --i)
            frame[headerLen - 1 - i] = static_cast<uint8_t>((len >> (i * 8)) & 0xFF);
    }

    if (data && len > 0) {
        std::memcpy(frame.data() + headerLen, data, len);
    }
    ::send(sock, frame.data(), frame.size(), MSG_NOSIGNAL);
}

void WebSocketServer::sendText(int sock, const std::string& text) {
    sendFrame(sock, text.data(), text.size(), 0x01);
}

void WebSocketServer::sendBinary(int sock, const void* data, size_t len) {
    sendFrame(sock, data, len, 0x02);
}

/* ------------------------------------------------------------------ */
/*  Client loop                                                         */
/* ------------------------------------------------------------------ */

void WebSocketServer::clientLoop(int sock) {
    std::string request;
    request.reserve(1024);
    char buf[256];
    bool gotHeaders = false;

    while (m_running.load() && !gotHeaders) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(sock, &fds);
        struct timeval tv;
        tv.tv_sec = 5;
        tv.tv_usec = 0;
        int ret = select(sock + 1, &fds, nullptr, nullptr, &tv);
        if (ret <= 0) break;

        ssize_t n = ::recv(sock, buf, sizeof(buf), 0);
        if (n <= 0) break;
        request.append(buf, n);
        if (request.find("\r\n\r\n") != std::string::npos) gotHeaders = true;
    }

    if (!gotHeaders || request.empty()) {
        Logger::getInstance().warning("WebSocketServer: bad HTTP request");
        close(sock);
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        m_clients.erase(std::remove_if(m_clients.begin(), m_clients.end(),
            [&](const Client* c) { return c->sock == sock; }), m_clients.end());
        return;
    }

    if (!doHandshake(sock, request)) {
        close(sock);
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        m_clients.erase(std::remove_if(m_clients.begin(), m_clients.end(),
            [&](const Client* c) { return c->sock == sock; }), m_clients.end());
        return;
    }

    /* Find our client entry */
    Client* client = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        for (auto* c : m_clients) {
            if (c->sock == sock) { client = c; break; }
        }
    }
    if (!client) {
        Logger::getInstance().error("WebSocketServer: client not found after handshake");
        close(sock);
        return;
    }

    static const char* channelNames[10] = {
        "pgm-l", "pgm-r", "tweeter", "woofer",
        "mic-1", "mic-2", "mic-3", "mic-4", "mic-5", "mic-6"
    };

    /* Read frames — wait for subscribe, then keep alive */
    std::vector<uint8_t> payloadBuf(65536);
    while (m_running.load()) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(sock, &fds);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 200000;

        int ret = select(sock + 1, &fds, nullptr, nullptr, &tv);
        if (ret <= 0) continue;

        size_t payloadLen = 0;
        bool fin = false;
        uint8_t opcode = 0;
        if (!readFrame(sock, payloadBuf.data(), payloadLen, fin, opcode)) {
            Logger::getInstance().info("WebSocketServer: client disconnected");
            break;
        }

        if (opcode == 0x08) {
            Logger::getInstance().info("WebSocketServer: client sent close");
            sendFrame(sock, nullptr, 0, 0x08);
            break;
        }

        if (opcode == 0x09) {
            sendFrame(sock, payloadBuf.data(), payloadLen, 0x0A);
            continue;
        }

        if (opcode == 0x01 && !client->subscribed) {
            payloadBuf[payloadLen] = '\0';
            std::string text(reinterpret_cast<char*>(payloadBuf.data()), payloadLen);
            Logger::getInstance().info("WebSocketServer: subscribe request: " + text);

            std::string channelName;
            size_t chPos = text.find("\"channel\"");
            if (chPos != std::string::npos) {
                size_t colon = text.find(':', chPos);
                if (colon != std::string::npos) {
                    size_t q1 = text.find('"', colon + 1);
                    if (q1 != std::string::npos) {
                        size_t q2 = text.find('"', q1 + 1);
                        if (q2 != std::string::npos) {
                            channelName = text.substr(q1 + 1, q2 - q1 - 1);
                        }
                    }
                }
            }

            uint8_t channelId = 0;
            bool found = false;
            for (int i = 0; i < 10; ++i) {
                if (channelName == channelNames[i]) {
                    channelId = static_cast<uint8_t>(i);
                    found = true;
                    break;
                }
            }

            if (!found) {
                Logger::getInstance().warning("WebSocketServer: unknown channel '" + channelName + "'");
                sendText(sock, "{\"ok\":false,\"error\":\"Unknown channel: " + channelName + "\"}");
                break;
            }

            {
                std::lock_guard<std::mutex> lock(m_clientsMutex);
                client->channel = channelId;
                client->subscribed = true;
            }

            std::string ack = "{\"ok\":true,\"sampleRate\":" + std::to_string(m_sampleRate) +
                ",\"bufferSize\":" + std::to_string(m_bufferSize) + "}";
            sendText(sock, ack);
            Logger::getInstance().info("WebSocketServer: subscribed to " + channelName +
                " (ch=" + std::to_string(channelId) + ")");
        }
    }

    /* Cleanup */
    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        client->sock = -1;
        client->subscribed = false;
    }
    close(sock);

    std::lock_guard<std::mutex> lock(m_clientsMutex);
    auto it = std::remove_if(m_clients.begin(), m_clients.end(),
        [&](const Client* c) { return c->sock == sock; });
    for (auto it2 = it; it2 != m_clients.end(); ++it2) delete *it2;
    m_clients.erase(it, m_clients.end());
    Logger::getInstance().info("WebSocketServer: client removed (" +
        std::to_string(m_clients.size()) + " remaining)");
}
