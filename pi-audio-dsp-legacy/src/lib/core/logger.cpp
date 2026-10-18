#include "core/logger.h"
#include <iostream>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <algorithm>

Logger& Logger::getInstance() {
    static Logger instance;
    return instance;
}

void Logger::init(const std::string& logFile, LogLevel level,
                  size_t maxBytes, int maxBackups) {
    m_level = level;
    m_logFilePath = logFile;
    m_maxBytes = maxBytes;
    m_maxBackups = std::max(1, maxBackups);
    m_logFile.open(logFile, std::ios::app);
}

void Logger::rotate() {
    if (m_maxBytes == 0 || m_maxBackups <= 0) return;

    m_logFile.close();

    char buf[64];
    for (int i = m_maxBackups - 1; i >= 1; --i) {
        std::string src = m_logFilePath + "." + std::to_string(i);
        std::string dst = m_logFilePath + "." + std::to_string(i + 1);
        rename(src.c_str(), dst.c_str());
    }
    rename(m_logFilePath.c_str(), (m_logFilePath + ".1").c_str());
    m_logFile.open(m_logFilePath, std::ios::out | std::ios::trunc);
}

void Logger::log(LogLevel level, const std::string& message) {
    if (level < m_level) return;

    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_maxBytes > 0 && m_logFile.is_open()) {
        std::streampos pos = m_logFile.tellp();
        if (pos >= 0 && static_cast<size_t>(pos) >= m_maxBytes) {
            rotate();
        }
    }

    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    struct tm tm_buf;
    localtime_r(&time_t, &tm_buf);

    char timeBuf[64];
    strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", &tm_buf);

    std::string levelStr;
    switch (level) {
        case LogLevel::DEBUG: levelStr = "DEBUG"; break;
        case LogLevel::INFO: levelStr = "INFO"; break;
        case LogLevel::WARNING: levelStr = "WARNING"; break;
        case LogLevel::ERROR: levelStr = "ERROR"; break;
    }

    std::string formatted = std::string("[") + timeBuf + "] [" + levelStr + "] " + message;

    if (m_logFile.is_open()) {
        m_logFile << formatted << std::endl;
    }

    if (level == LogLevel::ERROR) {
        std::cerr << formatted << std::endl;
    }
}

void Logger::debug(const std::string& msg) { log(LogLevel::DEBUG, msg); }
void Logger::info(const std::string& msg) { log(LogLevel::INFO, msg); }
void Logger::warning(const std::string& msg) { log(LogLevel::WARNING, msg); }
void Logger::error(const std::string& msg) { log(LogLevel::ERROR, msg); }
