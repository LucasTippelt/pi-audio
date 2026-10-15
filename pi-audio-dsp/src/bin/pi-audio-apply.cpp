#include "adapters/apply.h"
#include "core/logger.h"
#include <iostream>
#include <string>

int main() {
    Logger::getInstance().init("/var/log/pi-audio/apply.log");

    std::string configFile = "/etc/pi-audio/config/current.json";

    std::string jsonInput;
    std::string line;
    while (std::getline(std::cin, line)) {
        jsonInput += line;
    }

    return applyAdapter(configFile);
}
