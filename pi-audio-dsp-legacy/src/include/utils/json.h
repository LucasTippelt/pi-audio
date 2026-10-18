#ifndef PI_AUDIO_JSON_H
#define PI_AUDIO_JSON_H

#include <string>
#include <map>
#include <vector>

// Simple JSON parsing and generation utilities
class JsonUtils {
public:
    static std::string parseJson(const std::string& json);
    static std::string generateJson(const std::map<std::string, std::string>& data);
    static std::string parseConfigJson(const std::string& json);
};

#endif
