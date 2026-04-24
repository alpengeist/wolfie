#pragma once

#include <string>

#include "core/models.h"

namespace wolfie::persistence {

bool loadMicrophoneCalibration(AudioSettings& settings, std::wstring& errorMessage);

}  // namespace wolfie::persistence
