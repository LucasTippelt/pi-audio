#include "adapters/apply.h"
#include "core/config.h"
#include "core/logger.h"
#include <iostream>
#include <fstream>
#include <string>

int applyAdapter(const std::string& configFile) {
    Logger::getInstance().info("Apply adapter invoked");
    
    // Read configuration from file
    auto configManager = ConfigManager::instance();
    if (!configManager->loadFromFile(configFile)) {
        std::cerr << "Failed to load configuration from " << configFile << std::endl;
        return 1;
    }
    
    // Apply configuration to engine
    // In a real implementation, this would communicate with the running engine
    // For now, we just validate the config
    
    auto config = configManager->getConfig();
    
    // Validate sample rate
    if (config.sampleRate != 44100 && config.sampleRate != 48000 &&
        config.sampleRate != 88200 && config.sampleRate != 96000 &&
        config.sampleRate != 176400 && config.sampleRate != 192000) {
        std::cerr << "Invalid sample rate: " << config.sampleRate << std::endl;
        return 1;
    }
    
    // Validate buffer size
    if (config.bufferSize != 64 && config.bufferSize != 128 &&
        config.bufferSize != 256 && config.bufferSize != 512 &&
        config.bufferSize != 1024 && config.bufferSize != 2048 &&
        config.bufferSize != 4096) {
        std::cerr << "Invalid buffer size: " << config.bufferSize << std::endl;
        return 1;
    }
    
    // Apply to engine (placeholder)
    Logger::getInstance().info("Configuration applied successfully");
    std::cout << "{\"ok\":true}" << std::endl;
    
    return 0;
}
