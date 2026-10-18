#include "adapters/apply.h"
#include "core/config.h"
#include "core/logger.h"
#include <iostream>
#include <fstream>
#include <string>
#include <cstdio>
#include <chrono>
#include <thread>
#include <cstdlib>

int applyAdapter(const std::string& configFile) {
    Logger::getInstance().info("Apply adapter invoked");

    auto configManager = ConfigManager::instance();
    if (!configManager->loadFromFile(configFile)) {
        std::cerr << "Failed to load configuration from " << configFile << std::endl;
        return 1;
    }

    auto config = configManager->getConfig();

    if (config.sampleRate != 44100 && config.sampleRate != 48000 &&
        config.sampleRate != 88200 && config.sampleRate != 96000 &&
        config.sampleRate != 176400 && config.sampleRate != 192000) {
        std::cerr << "Invalid sample rate: " << config.sampleRate << std::endl;
        return 1;
    }

    if (config.bufferSize != 64 && config.bufferSize != 128 &&
        config.bufferSize != 256 && config.bufferSize != 512 &&
        config.bufferSize != 1024 && config.bufferSize != 2048 &&
        config.bufferSize != 4096) {
        std::cerr << "Invalid buffer size: " << config.bufferSize << std::endl;
        return 1;
    }

    FILE* f = fopen("/run/pi-audio/reload", "w");
    if (f) {
        fprintf(f, "%s", configFile.c_str());
        fclose(f);
        Logger::getInstance().info("Reload trigger written");
    } else {
        Logger::getInstance().warning("Failed to write reload trigger");
    }

    const int maxWaitMs = 5000;
    const int pollMs = 100;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(maxWaitMs);

    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(pollMs));
        std::ifstream stateFile("/run/pi-audio/engine-state.json");
        if (!stateFile.is_open()) continue;

        std::string stateContent((std::istreambuf_iterator<char>(stateFile)),
                                   std::istreambuf_iterator<char>());
        stateFile.close();

        size_t srPos = stateContent.find("\"sampleRate\":");
        size_t bsPos = stateContent.find("\"BufferSize\":");
        if (srPos == std::string::npos || bsPos == std::string::npos) continue;

        srPos += 13;
        while (srPos < stateContent.length() && (stateContent[srPos] == ' ' || stateContent[srPos] == '\t')) srPos++;
        std::string srStr;
        while (srPos < stateContent.length() && stateContent[srPos] >= '0' && stateContent[srPos] <= '9') {
            srStr += stateContent[srPos++];
        }

        bsPos += 13;
        while (bsPos < stateContent.length() && (stateContent[bsPos] == ' ' || stateContent[bsPos] == '\t')) bsPos++;
        std::string bsStr;
        while (bsPos < stateContent.length() && stateContent[bsPos] >= '0' && stateContent[bsPos] <= '9') {
            bsStr += stateContent[bsPos++];
        }

        if (srStr.empty() || bsStr.empty()) continue;

        int confirmedSr = std::atoi(srStr.c_str());
        int confirmedBs = std::atoi(bsStr.c_str());

        if (confirmedSr == config.sampleRate && confirmedBs == config.bufferSize) {
            Logger::getInstance().info("Engine confirmed: sampleRate=" + srStr + ", BufferSize=" + bsStr);
            break;
        }
    }

    Logger::getInstance().info("Configuration applied successfully");
    std::cout << "{\"ok\":true}" << std::endl;
    return 0;
}
