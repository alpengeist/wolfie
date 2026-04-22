#pragma once

#include <memory>

#include "audio/audio_backend.h"

namespace wolfie::audio {

std::unique_ptr<IAudioBackend> createWinMmAudioBackend();

}  // namespace wolfie::audio
