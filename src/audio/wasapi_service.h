#pragma once

#include <string>
#include <vector>

namespace wolfie::audio {

struct WasapiDevice {
    std::string id;
    std::wstring name;
    int channelCount = 0;
};

class WasapiService {
public:
    [[nodiscard]] std::vector<WasapiDevice> enumerateInputDevices() const;
    [[nodiscard]] std::vector<WasapiDevice> enumerateOutputDevices() const;
};

}  // namespace wolfie::audio
