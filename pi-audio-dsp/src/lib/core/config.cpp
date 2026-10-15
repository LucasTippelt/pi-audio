#include "core/config.h"
#include <fstream>
#include <iostream>
#include "utils/json.hpp"

std::shared_ptr<ConfigManager> ConfigManager::instance() {
    static std::shared_ptr<ConfigManager> inst(new ConfigManager());
    return inst;
}

bool ConfigManager::loadFromFile(const std::string& path) {
    std::lock_guard<std::mutex> lock(m_mutex);

    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "Failed to open config file: " << path << std::endl;
        return false;
    }

    try {
        json j;
        file >> j;

        m_config.input = j.value("input", "default");
        m_config.output = j.value("output", "default");
        m_config.sampleRate = j.value("sampleRate", 48000);
        m_config.bufferSize = j.value("BufferSize", 512);
        m_config.micCount = j.value("micCount", 2);
        m_config.volume = j.value("volume", 65);
        m_config.muted = j.value("muted", false);

        if (j.contains("bands")) {
            for (auto& [key, bandArray] : j["bands"].items()) {
                for (auto& band : bandArray) {
                    BandConfig bc;
                    bc.id = band.value("id", "");
                    bc.enabled = band.value("enabled", true);
                    bc.frequency = band.value("frequency", 1000.0f);
                    bc.q = band.value("q", 1.0f);
                    bc.gain = band.value("gain", 0.0f);
                    m_config.bands[key] = bc;
                }
            }
        }

        if (j.contains("drivers")) {
            for (auto& driver : j["drivers"]) {
                DriverConfig dc;
                dc.name = driver.value("name", "");
                dc.delayMs = driver.value("delayMs", 0.0f);
                dc.trimDb = driver.value("trimDb", 0.0f);
                dc.phaseInverted = driver.value("phaseInverted", false);
                m_config.drivers.push_back(dc);
            }
        }

        if (j.contains("limiter")) {
            m_config.limiter.enabled = j["limiter"].value("enabled", true);
            m_config.limiter.threshold = j["limiter"].value("threshold", -1.0f);
            m_config.limiter.release = j["limiter"].value("release", 80);
        }

        return true;
    } catch (const std::exception& e) {
        std::cerr << "Failed to parse config: " << e.what() << std::endl;
        return false;
    }
}

bool ConfigManager::saveToFile(const std::string& path) {
    std::lock_guard<std::mutex> lock(m_mutex);

    std::ofstream file(path);
    if (!file.is_open()) {
        std::cerr << "Failed to open config file for writing: " << path << std::endl;
        return false;
    }

    json j;
    j["input"] = m_config.input;
    j["output"] = m_config.output;
    j["sampleRate"] = m_config.sampleRate;
    j["BufferSize"] = m_config.bufferSize;
    j["micCount"] = m_config.micCount;
    j["volume"] = m_config.volume;
    j["muted"] = m_config.muted;

    json bandsJson = json::object();
    for (auto& [key, band] : m_config.bands) {
        json bandJson;
        bandJson["id"] = band.id;
        bandJson["enabled"] = band.enabled;
        bandJson["frequency"] = band.frequency;
        bandJson["q"] = band.q;
        bandJson["gain"] = band.gain;
        bandsJson[key].push_back(bandJson);
    }
    j["bands"] = bandsJson;

    json driversJson = json::array();
    for (auto& driver : m_config.drivers) {
        json driverJson;
        driverJson["name"] = driver.name;
        driverJson["delayMs"] = driver.delayMs;
        driverJson["trimDb"] = driver.trimDb;
        driverJson["phaseInverted"] = driver.phaseInverted;
        driversJson.push_back(driverJson);
    }
    j["drivers"] = driversJson;

    json limiterJson;
    limiterJson["enabled"] = m_config.limiter.enabled;
    limiterJson["threshold"] = m_config.limiter.threshold;
    limiterJson["release"] = m_config.limiter.release;
    j["limiter"] = limiterJson;

    file << j.dump(2);
    return true;
}
