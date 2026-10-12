#ifndef PI_AUDIO_CONFIG_H
#define PI_AUDIO_CONFIG_H

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <mutex>

struct BandConfig {
    std::string id;
    bool enabled = true;
    float frequency = 1000.0f;
    float q = 1.0f;
    float gain = 0.0f;
};

struct DriverConfig {
    std::string name;
    float delayMs = 0.0f;
    float trimDb = 0.0f;
    bool phaseInverted = false;
};

struct LimiterConfig {
    bool enabled = true;
    float threshold = -1.0f;
    int release = 80;
};

struct MeasurementConfig {
    std::string measuredAt;
    struct ResponsePoint {
        float hz = 0.0f;
        float db = 0.0f;
    };
    std::vector<ResponsePoint> left;
    std::vector<ResponsePoint> right;
};

struct FullConfig {
    std::string input = "default";
    std::string output = "default";
    int sampleRate = 48000;
    int bufferSize = 512;
    int volume = 65;
    bool muted = false;
    
    std::map<std::string, BandConfig> bands;
    std::vector<DriverConfig> drivers;
    LimiterConfig limiter;
    MeasurementConfig measurement;
};

class ConfigManager {
public:
    static std::shared_ptr<ConfigManager> instance();

    bool loadFromFile(const std::string& path);
    bool saveToFile(const std::string& path);

    FullConfig getConfig() const { return m_config; }
    void setConfig(const FullConfig& config) { m_config = config; }

    ConfigManager() = default;

private:
    FullConfig m_config;
    std::mutex m_mutex;
};

#endif
