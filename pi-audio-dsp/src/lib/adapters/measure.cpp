#include "adapters/measure.h"
#include "core/logger.h"
#include <iostream>

int measureAdapter() {
    Logger::getInstance().info("Measure adapter invoked");
    Logger::getInstance().warning("Room measurement is not yet implemented");

    std::cout << "{" << std::endl;
    std::cout << "  \"error\": \"Room measurement is not yet implemented.\"," << std::endl;
    std::cout << "  \"message\": \"Connect a measurement microphone and implement RTA/impulse response capture to enable automatic room correction.\"" << std::endl;
    std::cout << "}" << std::endl;

    return 1;
}
