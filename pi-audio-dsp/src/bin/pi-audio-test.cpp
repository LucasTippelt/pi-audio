#include "adapters/test.h"
#include "core/logger.h"
#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
    Logger::getInstance().init("/var/log/pi-audio/test.log");

    std::string driver = "Woofer_L";
    if (argc >= 2) {
        driver = argv[1];
    } else {
        std::string jsonInput;
        std::string line;
        while (std::getline(std::cin, line)) {
            jsonInput += line;
        }
        if (jsonInput.find("Woofer_L") != std::string::npos) driver = "Woofer_L";
        else if (jsonInput.find("Tweeter_L") != std::string::npos) driver = "Tweeter_L";
        else if (jsonInput.find("Woofer_R") != std::string::npos) driver = "Woofer_R";
        else if (jsonInput.find("Tweeter_R") != std::string::npos) driver = "Tweeter_R";
        else if (jsonInput.find("Mic") != std::string::npos) driver = "Mic";
    }

    return testAdapter(driver);
}
