#include "adapters/apply.h"
#include "core/logger.h"
#include <iostream>
#include <string>

int main() {
    Logger::getInstance().init("/var/log/pi-audio/apply.log");
    
    std::string configFile = "/etc/pi-audio/config/current.json";
    
    // Read JSON from stdin
    std::string jsonInput;
    std::string line;
    while (std::getline(std::cin, line)) {
        jsonInput += line;
    }
    
    // In a real implementation, we would parse the JSON and validate it
    // For now, we just apply the config from the file
    
    return applyAdapter(configFile);
}
