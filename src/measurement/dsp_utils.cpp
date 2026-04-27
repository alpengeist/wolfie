#include "measurement/dsp_utils.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace wolfie::measurement {

void fft(std::vector<std::complex<double>>& samples, bool inverse) {
    const size_t count = samples.size();
    if (count <= 1) {
        return;
    }

    for (size_t i = 1, j = 0; i < count; ++i) {
        size_t bit = count >> 1U;
        while ((j & bit) != 0) {
            j ^= bit;
            bit >>= 1U;
        }
        j ^= bit;
        if (i < j) {
            std::swap(samples[i], samples[j]);
        }
    }

    for (size_t length = 2; length <= count; length <<= 1U) {
        const double angle = (inverse ? 2.0 : -2.0) * std::numbers::pi_v<double> / static_cast<double>(length);
        const std::complex<double> phaseStep(std::cos(angle), std::sin(angle));
        for (size_t offset = 0; offset < count; offset += length) {
            std::complex<double> phase(1.0, 0.0);
            const size_t halfLength = length >> 1U;
            for (size_t i = 0; i < halfLength; ++i) {
                const std::complex<double> even = samples[offset + i];
                const std::complex<double> odd = samples[offset + i + halfLength] * phase;
                samples[offset + i] = even + odd;
                samples[offset + i + halfLength] = even - odd;
                phase *= phaseStep;
            }
        }
    }

    if (inverse) {
        const double scale = 1.0 / static_cast<double>(count);
        for (std::complex<double>& sample : samples) {
            sample *= scale;
        }
    }
}

size_t nextPowerOfTwo(size_t value) {
    size_t power = 1;
    while (power < std::max<size_t>(value, 1)) {
        power <<= 1U;
    }
    return power;
}

std::vector<double> unwrapPhaseRadians(const std::vector<double>& phaseRadians) {
    std::vector<double> unwrapped = phaseRadians;
    if (unwrapped.empty()) {
        return unwrapped;
    }

    double phaseOffset = 0.0;
    for (size_t index = 1; index < unwrapped.size(); ++index) {
        const double delta = phaseRadians[index] - phaseRadians[index - 1];
        if (delta > std::numbers::pi_v<double>) {
            phaseOffset -= 2.0 * std::numbers::pi_v<double>;
        } else if (delta < -std::numbers::pi_v<double>) {
            phaseOffset += 2.0 * std::numbers::pi_v<double>;
        }
        unwrapped[index] = phaseRadians[index] + phaseOffset;
    }
    return unwrapped;
}

std::vector<double> buildLogFrequencyAxis(double minFrequencyHz, double maxFrequencyHz, size_t pointCount) {
    std::vector<double> axis;
    if (pointCount == 0 || maxFrequencyHz <= minFrequencyHz) {
        return axis;
    }

    const double logMin = std::log10(std::max(minFrequencyHz, 1.0));
    const double logMax = std::log10(std::max(maxFrequencyHz, minFrequencyHz + 1.0));
    axis.reserve(pointCount);
    for (size_t index = 0; index < pointCount; ++index) {
        const double t = pointCount <= 1 ? 0.0 : static_cast<double>(index) / static_cast<double>(pointCount - 1);
        axis.push_back(std::pow(10.0, logMin + ((logMax - logMin) * t)));
    }
    return axis;
}

std::vector<double> buildLinearFrequencyAxis(int sampleRate, size_t fftSize) {
    const size_t positiveBinCount = (fftSize / 2) + 1;
    std::vector<double> axis;
    axis.reserve(positiveBinCount);
    for (size_t index = 0; index < positiveBinCount; ++index) {
        axis.push_back(static_cast<double>(sampleRate) * static_cast<double>(index) /
                       static_cast<double>(std::max<size_t>(fftSize, 1)));
    }
    return axis;
}

}  // namespace wolfie::measurement
