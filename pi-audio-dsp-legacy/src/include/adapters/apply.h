#ifndef PI_AUDIO_APPLY_H
#define PI_AUDIO_APPLY_H

#include <string>

// Adapter that applies configuration from current.json to the running engine
int applyAdapter(const std::string& configFile);

#endif
