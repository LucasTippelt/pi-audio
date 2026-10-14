#ifndef PI_AUDIO_LOGGER_H
#define PI_AUDIO_LOGGER_H

#include <string>
#include <mutex>
#include <fstream>

enum class LogLevel {
    DEBUG,
    INFO,
    WARNING,
    ERROR
};

class Logger {
public:
    static Logger& getInstance();

    void init(const std::string& logFile, LogLevel level = LogLevel::INFO,
              size_t maxBytes = 1024 * 1024, int maxBackups = 3);
    void log(LogLevel level, const std::string& message);
    void debug(const std::string& msg);
    void info(const std::string& msg);
    void warning(const std::string& msg);
    void error(const std::string& msg);

private:
    Logger() = default;
    void rotate();
    std::string m_logFilePath;
    std::ofstream m_logFile;
    LogLevel m_level = LogLevel::INFO;
    std::mutex m_mutex;
    size_t m_maxBytes = 0;
    int m_maxBackups = 0;
};

#endif
