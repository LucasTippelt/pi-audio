#include "core/logger.h"
#include <iostream>
#include <chrono>
#include <ctime>

Logger& Logger::getInstance() {
    static Logger instance;
    return instance;
}

void Logger::init(const std::string& logFile, LogLevel level) {
    m_level = level;
    m_logFile.open(logFile, std::ios::app);
}

void Logger::log(LogLevel level, const std::string& message) {
    if (level < m_level) return;
    
    std::lock_guard<std::mutex> lock(m_mutex);
    
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
    
    // Also print to stderr for ERROR level
    if (level == LogLevel::ERROR) {
        std::cerr << formatted << std::endl;
    }
}

void Logger::debug(const std::string& msg) { log(LogLevel::DEBUG, msg); }
void Logger::info(const std::string& msg) { log(LogLevel::INFO, msg); }
void Logger::warning(const std::string& msg) { log(LogLevel::WARNING, msg); }
void Logger::error(const std::string& msg) { log(LogLevel::ERROR, msg); }
