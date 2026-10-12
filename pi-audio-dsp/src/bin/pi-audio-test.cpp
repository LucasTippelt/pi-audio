#include "adapters/test.h"
#include "core/logger.h"
#include <iostream>
#include <string>

int main() {
    Logger::getInstance().init("/var/log/pi-audio/test.log");
    
    // Read driver from stdin JSON
    std::string jsonInput;
    std::string line;
    while (std::getline(std::cin, line)) {
        jsonInput += line;
    }
    
    // Parse driver name (simplified)
    std::string driver = "Woofer_L";
    if (jsonInput.find("Woofer_L") != std::string::npos) {
        driver = "Woofer_L";
    } else if (jsonInput.find("Tweeter_L") != std::string::npos) {
        driver = "Tweeter_L";
    } else if (jsonInput.find("Woofer_R") != std::string::npos) {
        driver = "Woofer_R";
    } else if (jsonInput.find("Tweeter_R") != std::string::npos) {
        driver = "Tweeter_R";
    }
    
    return testAdapter(driver);
}
