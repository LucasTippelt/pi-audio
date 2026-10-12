#include "adapters/status.h"
#include "core/logger.h"
#include <iostream>

int main() {
    Logger::getInstance().init("/var/log/pi-audio/status.log");
    
    return statusAdapter();
}
