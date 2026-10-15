#include "adapters/test.h"
#include "core/logger.h"
#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
    Logger::getInstance().init("/var/log/pi-audio/test.log");

    if (argc < 2) {
        std::cerr << "Usage: pi-audio-test <driver>" << std::endl;
        std::cerr << "Drivers: Tweeter, Woofer, PGM_L, PGM_R, Mic_1..Mic_6" << std::endl;
        return 1;
    }

    return testAdapter(argv[1]);
}
