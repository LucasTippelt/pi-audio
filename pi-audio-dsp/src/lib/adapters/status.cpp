#include "adapters/status.h"
#include "core/config.h"
#include "core/logger.h"
#include <iostream>
#include <fstream>
#include <string>

int statusAdapter() {
    Logger::getInstance().info("Status probe invoked");
    
    // Check if engine is running by reading status file
    // In a real implementation, this would check the engine's state
    // For now, we simulate a running engine
    
    std::ifstream statusFile("/run/pi-audio/status");
    bool connected = statusFile.is_open();
    
    if (connected) {
        statusFile.close();
        // Read actual sample rate and buffer size from engine
        // For now, we return default values
        std::cout << "{\"connected\":true,\"sampleRate\":48000,\"BufferSize\":512}" << std::endl;
    } else {
        std::cout << "{\"connected\":false,\"sampleRate\":null,\"BufferSize\":null,\"message\":\"Engine not running\"}" << std::endl;
    }
    
    return 0;
}
