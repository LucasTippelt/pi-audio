#include "adapters/status.h"
#include "core/config.h"
#include "core/logger.h"
#include <iostream>
#include <fstream>
#include <string>
#include <sstream>

int statusAdapter() {
    Logger::getInstance().info("Status probe invoked");

    std::ifstream statusFile("/run/pi-audio/status");
    bool connected = statusFile.is_open();

    if (connected) {
        statusFile.close();

        std::ifstream stateFile("/run/pi-audio/engine-state.json");
        if (stateFile.is_open()) {
            std::stringstream buffer;
            buffer << stateFile.rdbuf();
            stateFile.close();
            std::string stateJson = buffer.str();
            // Strip outer braces to merge into single JSON object
            if (stateJson.size() >= 2 && stateJson.front() == '{' && stateJson.back() == '}') {
                std::string inner = stateJson.substr(1, stateJson.size() - 2);
                std::cout << "{\"connected\":true," << inner << "}" << std::endl;
            } else {
                std::cout << "{\"connected\":true,\"sampleRate\":48000,\"BufferSize\":512}" << std::endl;
            }
        } else {
            std::cout << "{\"connected\":true,\"sampleRate\":48000,\"BufferSize\":512}" << std::endl;
        }
    } else {
        std::cout << "{\"connected\":false,\"sampleRate\":null,\"BufferSize\":null,\"message\":\"Engine not running\"}" << std::endl;
    }

    return 0;
}
