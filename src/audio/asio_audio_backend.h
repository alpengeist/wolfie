#pragma once

#include <memory>

#include "audio/audio_backend.h"

namespace wolfie::audio {

std::unique_ptr<IAudioBackend> createAsioAudioBackend();
std::unique_ptr<IAudioBackend> createDefaultAudioBackend();

}  // namespace wolfie::audio
