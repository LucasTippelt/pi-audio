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
    
    void init(const std::string& logFile, LogLevel level = LogLevel::INFO);
    void log(LogLevel level, const std::string& message);
    void debug(const std::string& msg);
    void info(const std::string& msg);
    void warning(const std::string& msg);
    void error(const std::string& msg);

private:
    Logger() = default;
    std::ofstream m_logFile;
    LogLevel m_level = LogLevel::INFO;
    std::mutex m_mutex;
};

#endif
