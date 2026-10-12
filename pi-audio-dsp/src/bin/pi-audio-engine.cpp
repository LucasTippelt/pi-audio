#include "core/engine.h"
#include "core/config.h"
#include "core/logger.h"
#include <iostream>
#include <fstream>
#include <string>
#include <signal.h>

static AudioEngine* g_engine = nullptr;

void signalHandler(int signum) {
    if (g_engine) {
        g_engine->stop();
    }
    std::remove("/run/pi-audio/status");
    exit(0);
}

int main(int argc, char* argv[]) {
    std::string configFile = "/etc/pi-audio/config/current.json";
    std::string logFile = "/var/log/pi-audio/engine.log";
    
    if (argc > 1) {
        configFile = argv[1];
    }
    if (argc > 2) {
        logFile = argv[2];
    }
    
    Logger::getInstance().init(logFile, LogLevel::DEBUG);
    Logger::getInstance().info("Starting Pi Audio Engine");
    
    // Load configuration
    auto configManager = ConfigManager::instance();
    if (!configManager->loadFromFile(configFile)) {
        Logger::getInstance().error("Failed to load configuration");
        return 1;
    }
    
    auto config = configManager->getConfig();
    EngineConfig engineConfig;
    engineConfig.input = config.input;
    engineConfig.output = config.output;
    engineConfig.sampleRate = config.sampleRate;
    engineConfig.bufferSize = config.bufferSize;
    engineConfig.volume = config.volume;
    engineConfig.muted = config.muted;
    
    // Initialize engine
    g_engine = new AudioEngine();
    if (!g_engine->initialize(engineConfig)) {
        Logger::getInstance().error("Failed to initialize audio engine");
        delete g_engine;
        return 1;
    }
    
    // Setup signal handlers
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    
    Logger::getInstance().info("Engine initialized successfully");

    // Create status file for web interface
    {
        std::ofstream statusFile("/run/pi-audio/status");
        if (statusFile.is_open()) {
            statusFile << "running";
            statusFile.close();
        }
    }

    // Start playback
    g_engine->start();
    Logger::getInstance().info("Playback started");
    
    // Keep running until stopped
    while (g_engine->isRunning()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    
    delete g_engine;
    g_engine = nullptr;

    // Remove status file
    std::remove("/run/pi-audio/status");

    Logger::getInstance().info("Engine stopped");
    return 0;
}
