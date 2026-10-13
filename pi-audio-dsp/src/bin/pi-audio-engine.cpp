#include "core/engine.h"
#include "core/config.h"
#include "core/logger.h"
#include <iostream>
#include <fstream>
#include <string>
#include <signal.h>
#include <sstream>
#include <cstdio>
#include <sys/stat.h>
#include <unistd.h>

static AudioEngine* g_engine = nullptr;
static EngineConfig g_engineConfig;
static std::string g_configFile;

static void writeEngineState() {
    if (!g_engine) return;
    auto cfg = g_engine->getCurrentConfig();
    std::ostringstream json;
    json << "{"
         << "\"input\":\"" << cfg.input << "\","
         << "\"output\":\"" << cfg.output << "\","
         << "\"sampleRate\":" << cfg.sampleRate << ","
         << "\"BufferSize\":" << cfg.bufferSize << ","
         << "\"volume\":" << g_engine->getVolume() << ","
         << "\"muted\":" << (g_engine->isMuted() ? "true" : "false")
         << "}";
    const char* tmpPath = "/run/pi-audio/engine-state.json.tmp";
    const char* finalPath = "/run/pi-audio/engine-state.json";
    FILE* f = fopen(tmpPath, "w");
    if (f) {
        fputs(json.str().c_str(), f);
        fclose(f);
        chmod(tmpPath, 0644);
        rename(tmpPath, finalPath);
    }
}

static EngineConfig loadEngineConfig(const std::string& path) {
    auto cm = ConfigManager::instance();
    if (cm->loadFromFile(path)) {
        auto fc = cm->getConfig();
        EngineConfig ec;
        ec.input = fc.input;
        ec.output = fc.output;
        ec.sampleRate = fc.sampleRate;
        ec.bufferSize = fc.bufferSize;
        ec.volume = fc.volume;
        ec.muted = fc.muted;
        return ec;
    }
    return g_engineConfig;
}

static void reloadFromConfig() {
    if (!g_engine) return;
    EngineConfig newCfg = loadEngineConfig(g_configFile);
    if (!newCfg.input.empty()) {
        g_engine->setVolume(newCfg.volume);
        g_engine->setMuted(newCfg.muted);

        bool needRestart = (newCfg.sampleRate != g_engineConfig.sampleRate ||
                            newCfg.bufferSize != g_engineConfig.bufferSize ||
                            newCfg.input != g_engineConfig.input ||
                            newCfg.output != g_engineConfig.output);

        if (needRestart) {
            Logger::getInstance().info("Reloading: engine restart required");
            g_engine->stop();
            if (g_engine->initialize(newCfg)) {
                g_engine->start();
                g_engineConfig = newCfg;
                Logger::getInstance().info("Engine reinitialized successfully");
            } else {
                Logger::getInstance().error("Reinit failed, keeping old config");
                g_engine->start();
            }
        } else {
            g_engineConfig.volume = newCfg.volume;
            g_engineConfig.muted = newCfg.muted;
            Logger::getInstance().info("Reloading: volume/mute updated");
        }
    }
}

void signalHandler(int signum) {
    if (g_engine) {
        g_engine->stop();
    }
    std::remove("/run/pi-audio/status");
    std::remove("/run/pi-audio/engine-state.json");
    std::remove("/run/pi-audio/reload");
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

    g_configFile = configFile;

    Logger::getInstance().init(logFile, LogLevel::DEBUG, 1024 * 1024, 3);
    Logger::getInstance().info("Starting Pi Audio Engine");

    // Load configuration
    auto configManager = ConfigManager::instance();
    if (!configManager->loadFromFile(configFile)) {
        Logger::getInstance().error("Failed to load configuration");
        return 1;
    }

    EngineConfig engineConfig = loadEngineConfig(configFile);

    // Initialize engine
    g_engine = new AudioEngine();
    if (!g_engine->initialize(engineConfig)) {
        Logger::getInstance().error("Failed to initialize audio engine");
        delete g_engine;
        g_engine = nullptr;
        return 1;
    }

    g_engineConfig = engineConfig;

    // Setup signal handlers
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    Logger::getInstance().info("Engine initialized successfully");

    // Create status file for web interface
    {
        FILE* f = fopen("/run/pi-audio/status", "w");
        if (f) {
            fputs("running", f);
            fclose(f);
            chmod("/run/pi-audio/status", 0644);
        }
    }

    writeEngineState();

    // Start playback
    g_engine->start();
    Logger::getInstance().info("Playback started");

    // Keep running until stopped
    while (g_engine->isRunning()) {
        // Check for reload trigger from apply adapter
        struct stat st;
        if (stat("/run/pi-audio/reload", &st) == 0) {
            Logger::getInstance().info("Reload trigger received");
            reloadFromConfig();
            std::remove("/run/pi-audio/reload");
        }
        writeEngineState();
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    delete g_engine;
    g_engine = nullptr;

    // Remove status file
    std::remove("/run/pi-audio/status");

    Logger::getInstance().info("Engine stopped");
    return 0;
}
