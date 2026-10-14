#include "core/http_stream.h"
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
#include <iostream>

HttpStreamServer::HttpStreamServer() {}

HttpStreamServer::~HttpStreamServer() {
    stop();
}

bool HttpStreamServer::start(const std::string& host, int port) {
    m_serverSock = socket(AF_INET, SOCK_STREAM, 0);
    if (m_serverSock < 0) {
        Logger::getInstance().error("HttpStreamServer: socket() failed: " + std::string(strerror(errno)));
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
        Logger::getInstance().info("HttpStreamServer: binding to 0.0.0.0:" + std::to_string(port));
    } else {
        inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
        Logger::getInstance().info("HttpStreamServer: binding to " + host + ":" + std::to_string(port));
    }

    if (::bind(m_serverSock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        Logger::getInstance().error("HttpStreamServer: bind failed on port " +
            std::to_string(port) + " — errno=" + std::to_string(errno) + " (" + strerror(errno) + ")");
        close(m_serverSock);
        m_serverSock = -1;
        return false;
    }
    Logger::getInstance().info("HttpStreamServer: bind OK");

    if (::listen(m_serverSock, 16) < 0) {
        Logger::getInstance().error("HttpStreamServer: listen failed: " + std::string(strerror(errno)));
        close(m_serverSock);
        m_serverSock = -1;
        return false;
    }

    m_running = true;
    m_acceptThread = std::thread(&HttpStreamServer::acceptLoop, this);
    Logger::getInstance().info("HttpStreamServer: listening on port " + std::to_string(port) + " (accept thread started)");
    return true;
}

void HttpStreamServer::stop() {
    Logger::getInstance().info("HttpStreamServer: stopping (" +
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
    Logger::getInstance().info("HttpStreamServer: stopped");
}

void HttpStreamServer::pushSamples(uint8_t channelId, const float* data, size_t frames) {
    if (!m_running.load() || !data || frames == 0) return;

    size_t dataLen = frames * sizeof(float);

    char hexLen[32];
    int hexLenSize = std::snprintf(hexLen, sizeof(hexLen), "%zx\r\n", dataLen);
    const char* crlf = "\r\n";

    auto tNow = std::chrono::steady_clock::now();
    double nowMs = std::chrono::duration<double, std::milli>(tNow.time_since_epoch()).count();
    auto tStart = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    double lockUs = std::chrono::duration<double, std::micro>(tStart - tNow).count();
    std::vector<Client*> dead;
    int delivered = 0, dropped = 0, eagainHits = 0;

    for (auto* c : m_clients) {
        if (!c->streaming || c->channel != channelId || c->sock < 0) continue;

        bool ok = true;
        if (hexLenSize > 0) {
            ssize_t n = ::send(c->sock, hexLen, hexLenSize, MSG_NOSIGNAL | MSG_DONTWAIT);
            if (n <= 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    ok = false; eagainHits++;
                } else {
                    dead.push_back(c);
                }
            }
        }
        if (ok) {
            ssize_t n = ::send(c->sock,
                reinterpret_cast<const char*>(data),
                dataLen,
                MSG_NOSIGNAL | MSG_DONTWAIT);
            if (n <= 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    ok = false; eagainHits++;
                } else {
                    dead.push_back(c);
                }
            } else if (n < static_cast<ssize_t>(dataLen)) {
                ok = false; eagainHits++;
            }
        }
        if (ok) {
            ssize_t n = ::send(c->sock, crlf, 2, MSG_NOSIGNAL | MSG_DONTWAIT);
            if (n <= 0) {
                if (!(errno == EAGAIN || errno == EWOULDBLOCK)) {
                    dead.push_back(c);
                } else {
                    eagainHits++;
                }
            }
        }

        if (ok) {
            c->bytesSent.fetch_add(dataLen, std::memory_order_relaxed);
            c->chunksSent.fetch_add(1, std::memory_order_relaxed);
            double gap = nowMs - c->lastPushMs;
            c->lastPushMs = nowMs;
            c->lastPushSeq++;
            delivered++;
        } else if (!std::any_of(dead.begin(), dead.end(), [&](const Client* d){ return d == c; })) {
            c->chunksDropped.fetch_add(1, std::memory_order_relaxed);
            c->eagainCount.fetch_add(1, std::memory_order_relaxed);
            dropped++;
        }
    }

    auto tEnd = std::chrono::steady_clock::now();
    double totalUs = std::chrono::duration<double, std::micro>(tEnd - tNow).count();

    if (dropped > 0 || eagainHits > 0) {
        Logger::getInstance().warning(
            "HTTP push ch=" + std::to_string(channelId) +
            " frames=" + std::to_string(frames) +
            " delivered=" + std::to_string(delivered) +
            " dropped=" + std::to_string(dropped) +
            " eagain=" + std::to_string(eagainHits) +
            " lock=" + std::to_string((int)lockUs) + "us" +
            " total=" + std::to_string((int)totalUs) + "us");
    }

    for (auto* c : dead) {
        if (c->sock >= 0) close(c->sock);
        c->sock = -1;
        c->streaming = false;
        delete c;
    }
}

void HttpStreamServer::setStatusCallback(std::function<std::string()> cb) {
    m_statusCb = std::move(cb);
}

void HttpStreamServer::acceptLoop() {
    while (m_running.load()) {
        struct sockaddr_in addr;
        socklen_t addrLen = sizeof(addr);
        int clientSock = ::accept(m_serverSock, (struct sockaddr*)&addr, &addrLen);
        if (clientSock < 0) {
            if (m_running.load()) {
                char ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
                Logger::getInstance().warning("HttpStreamServer: accept failed: " +
                    std::string(strerror(errno)) + " (from " + std::string(ip) + ":" +
                    std::to_string(ntohs(addr.sin_port)) + ")");
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            continue;
        }

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
        Logger::getInstance().info("HttpStreamServer: connection from " +
            std::string(ip) + ":" + std::to_string(ntohs(addr.sin_port)));

        int opt = 1;
        setsockopt(clientSock, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
        int bufSize = 256 * 1024;
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

void HttpStreamServer::clientLoop(int sock) {
    std::string request;
    request.reserve(512);
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
        if (ret <= 0) {
            Logger::getInstance().warning("HttpStreamServer: client read timeout/error (ret=" +
                std::to_string(ret) + ")");
            break;
        }

        ssize_t n = ::recv(sock, buf, sizeof(buf), MSG_DONTWAIT);
        if (n <= 0) {
            Logger::getInstance().warning("HttpStreamServer: client recv failed (n=" +
                std::to_string(n) + ")");
            break;
        }
        request.append(buf, n);
        if (request.size() > 4 && (request.substr(0, 4) == "\r\n\r\n" ||
            request.find("\r\n\r\n") != std::string::npos)) {
            gotHeaders = true;
        }
    }

    if (request.empty()) {
        Logger::getInstance().warning("HttpStreamServer: empty request, closing");
        close(sock);
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        m_clients.erase(std::remove_if(m_clients.begin(), m_clients.end(),
            [&](const Client* c) { return c->sock == sock; }), m_clients.end());
        return;
    }

    /* Log first line of request */
    size_t eol = request.find('\n');
    std::string firstLine = eol != std::string::npos ? request.substr(0, eol) : request;
    Logger::getInstance().info("HttpStreamServer: request: " + firstLine);

    /* Parse method and path */
    std::string method, path;
    {
        size_t sp = request.find(' ');
        if (sp != std::string::npos) {
            method = request.substr(0, sp);
            size_t sp2 = request.find(' ', sp + 1);
            if (sp2 != std::string::npos) {
                std::string fullPath = request.substr(sp + 1, sp2 - sp - 1);
                size_t qm = fullPath.find('?');
                path = (qm != std::string::npos) ? fullPath.substr(0, qm) : fullPath;
            }
        }
    }

    auto sendResponse = [&](const std::string& headers, const std::string& body) {
        std::string resp = "HTTP/1.1 " + headers;
        resp += "Access-Control-Allow-Origin: *\r\n";
        resp += "Access-Control-Max-Age: 86400\r\n";
        resp += "Content-Length: ";
        resp += std::to_string(body.size()) + "\r\n\r\n";
        resp += body;
        ::send(sock, resp.data(), resp.size(), MSG_NOSIGNAL);
    };

    auto sendJson = [&](const std::string& json) {
        sendResponse("200 OK\r\nContent-Type: application/json\r\n", json);
    };

    if (method == "OPTIONS") {
        Logger::getInstance().info("HttpStreamServer: OPTIONS preflight for " + path);
        std::string resp =
            "HTTP/1.1 204 No Content\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Methods: GET, OPTIONS\r\n"
            "Access-Control-Allow-Headers: *\r\n"
            "Access-Control-Max-Age: 86400\r\n"
            "\r\n";
        ::send(sock, resp.data(), resp.size(), MSG_NOSIGNAL);
        close(sock);
        return;
    }

    if (path == "/" || path == "/channels") {
        std::string channels = "[\"pgm-l\",\"pgm-r\",\"tweeter\",\"woofer\","
            "\"mic-1\",\"mic-2\",\"mic-3\",\"mic-4\",\"mic-5\",\"mic-6\"]";
        sendJson(channels);
        Logger::getInstance().info("HttpStreamServer: served /channels");
    } else if (path == "/status") {
        if (m_statusCb) {
            sendJson(m_statusCb());
        } else {
            sendJson("{\"ok\":true}");
        }
        Logger::getInstance().info("HttpStreamServer: served /status");
    } else if (path == "/stats") {
        std::ostringstream oss;
        oss << "{\"clients\":[";
        bool first = true;
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        for (auto* c : m_clients) {
            if (!first) oss << ",";
            first = false;
            static const char* names[10] = {
                "pgm-l","pgm-r","tweeter","woofer",
                "mic-1","mic-2","mic-3","mic-4","mic-5","mic-6"
            };
            std::string chName = (c->channel < 10) ? names[c->channel] : "unknown";
            oss << "{\"ch\":\"" << chName << "\""
                << ",\"streaming\":" << (c->streaming ? "true" : "false")
                << ",\"bytesSent\":" << c->bytesSent.load()
                << ",\"chunksSent\":" << c->chunksSent.load()
                << ",\"chunksDropped\":" << c->chunksDropped.load()
                << ",\"eagain\":" << c->eagainCount.load()
                << "}";
        }
        oss << "]}";
        sendJson(oss.str());
        Logger::getInstance().info("HttpStreamServer: served /stats");
    } else if (path == "/stream") {
        uint8_t channel = 0;
        std::string chName = "pgm-l";
        /* Extract query: firstLine is "GET /stream?ch=tweeter HTTP/1.1" */
        {
            size_t sp1 = firstLine.find(' ');
            if (sp1 != std::string::npos) {
                size_t sp2 = firstLine.find(' ', sp1 + 1);
                size_t end = (sp2 != std::string::npos) ? sp2 : firstLine.size();
                std::string fullPath = firstLine.substr(sp1 + 1, end - sp1 - 1);
                size_t qm = fullPath.find('?');
                if (qm != std::string::npos) {
                    std::string query = fullPath.substr(qm + 1);
                    if (query.rfind("ch=", 0) == 0) {
                        chName = query.substr(3);
                    }
                }
            }
        }
        {
            Logger::getInstance().info("HttpStreamServer: stream channel='" + chName + "'");
            static const char* names[10] = {
                "pgm-l", "pgm-r", "tweeter", "woofer",
                "mic-1", "mic-2", "mic-3", "mic-4", "mic-5", "mic-6"
            };
            bool found = false;
            for (int i = 0; i < 10; i++) {
                if (chName == names[i]) { channel = (uint8_t)i; found = true; break; }
            }
            if (!found) {
                Logger::getInstance().warning("HttpStreamServer: unknown channel '" + chName + "'");
                sendResponse("400 Bad Request\r\nContent-Type: text/plain\r\n",
                    "Unknown channel: " + chName);
                close(sock);
                std::lock_guard<std::mutex> lock(m_clientsMutex);
                m_clients.erase(std::remove_if(m_clients.begin(), m_clients.end(),
                    [&](const Client* c) { return c->sock == sock; }), m_clients.end());
                return;
            }
        }

        Logger::getInstance().info("HttpStreamServer: stream started channel=" + chName + " (ch=" +
            std::to_string(channel) + ")");

        /* Register as streaming client */
        {
            std::lock_guard<std::mutex> lock(m_clientsMutex);
            Client* c = nullptr;
            for (auto* cl : m_clients) {
                if (cl->sock == sock) { c = cl; break; }
            }
            if (!c) {
                Logger::getInstance().error("HttpStreamServer: stream client not found in list!");
                close(sock);
                return;
            }
            c->channel = channel;
        }

        /* Send HTTP headers for chunked transfer */
        std::string headers =
            "HTTP/1.1 200 OK\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Content-Type: audio/x-raw\r\n"
            "Transfer-Encoding: chunked\r\n"
            "X-Audio-Format: float32le-mono\r\n"
            "Cache-Control: no-cache\r\n\r\n";
        ssize_t hdrSent = ::send(sock, headers.data(), headers.size(), MSG_NOSIGNAL);
        Logger::getInstance().info("HttpStreamServer: sent stream headers (" +
            std::to_string(hdrSent) + " bytes)");

        /* Mark as streaming — pushSamples() will now deliver data */
        {
            std::lock_guard<std::mutex> lock(m_clientsMutex);
            for (auto* cl : m_clients) {
                if (cl->sock == sock) { cl->streaming = true; break; }
            }
        }
        Logger::getInstance().info("HttpStreamServer: client marked streaming, waiting for data...");

        /* Wait for connection to close */
        int idleCount = 0;
        while (m_running.load()) {
            char dummy;
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(sock, &fds);
            struct timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = 100000;
            int ret = select(sock + 1, &fds, nullptr, nullptr, &tv);
            if (ret > 0) {
                ssize_t n = ::recv(sock, &dummy, 1, MSG_PEEK);
                if (n <= 0) break;
            }
            idleCount++;
            if (idleCount % 1000 == 0) {
                Logger::getInstance().info("HttpStreamServer: stream active (" +
                    std::to_string(idleCount / 1000) + "s idle)");
            }
        }

        Logger::getInstance().info("HttpStreamServer: stream ended for channel=" + chName);
        ::send(sock, "0\r\n\r\n", 5, MSG_NOSIGNAL);
    } else {
        Logger::getInstance().warning("HttpStreamServer: 404 for path=" + path);
        sendResponse("404 Not Found\r\nContent-Type: text/plain\r\n", "Not found");
    }

    close(sock);
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    auto it = std::remove_if(m_clients.begin(), m_clients.end(),
        [&](const Client* c) { return c->sock == sock; });
    for (auto it2 = it; it2 != m_clients.end(); ++it2) delete *it2;
    m_clients.erase(it, m_clients.end());
    Logger::getInstance().info("HttpStreamServer: client disconnected (" +
        std::to_string(m_clients.size()) + " remaining)");
}
