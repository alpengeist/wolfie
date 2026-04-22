#include "measurement/response_smoother.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace wolfie::measurement {

namespace {

constexpr char kErbModelName[] = "ERB auditory smoothing";
constexpr char kOctaveSlidingWindowModelName[] = "octave sliding window";
constexpr double kErbWindowEdgeSigma = 3.0;

template <typename T>
T clampValue(T value, T low, T high) {
    return std::max(low, std::min(value, high));
}

std::string toLowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string canonicalModelName(std::string_view modelName) {
    const std::string lowered = toLowerAscii(std::string(modelName));
    if (lowered == "octave sliding window" ||
        lowered == "1/12 octave sliding window" ||
        lowered == "1/12th sliding window smoothing" ||
        lowered == "1/12 sliding window smoothing") {
        return kOctaveSlidingWindowModelName;
    }
    return kErbModelName;
}

double resolutionFactorFromPercent(int resolutionPercent) {
    const double t = clampValue(static_cast<double>(resolutionPercent) / 100.0, 0.0, 1.0);
    if (t <= 0.5) {
        return 0.5 + t;
    }
    return 1.0 + ((t - 0.5) * 2.0);
}

double erbRate(double frequencyHz) {
    const double clampedFrequencyHz = std::max(1.0, frequencyHz);
    return 21.4 * std::log10(1.0 + (0.00437 * clampedFrequencyHz));
}

double amplitudeDbFromPower(double power) {
    if (power <= 1.0e-9) {
        return -90.0;
    }
    return clampValue(10.0 * std::log10(power), -90.0, 24.0);
}

double powerFromAmplitudeDb(double valueDb) {
    return std::pow(10.0, valueDb / 10.0);
}

double averagePowerBand(const std::vector<double>& frequencyAxisHz,
                        const std::vector<double>& sourceDb,
                        double lowFrequencyHz,
                        double highFrequencyHz,
                        size_t centerIndex) {
    double totalPower = 0.0;
    double totalWeight = 0.0;
    for (size_t i = 0; i < frequencyAxisHz.size() && i < sourceDb.size(); ++i) {
        if (frequencyAxisHz[i] < lowFrequencyHz || frequencyAxisHz[i] > highFrequencyHz) {
            continue;
        }

        totalPower += powerFromAmplitudeDb(sourceDb[i]);
        totalWeight += 1.0;
    }

    if (totalWeight <= 1.0e-9) {
        return powerFromAmplitudeDb(sourceDb[centerIndex]);
    }
    return totalPower / totalWeight;
}

double smoothedValueAt(const std::vector<double>& frequencyAxisHz,
                       const std::vector<double>& sourceDb,
                       size_t centerIndex,
                       double lowWindowCycles,
                       double highWindowCycles) {
    const double centerFrequencyHz = std::max(1.0, frequencyAxisHz[centerIndex]);
    const double lowWidthHz = std::max(1.0, centerFrequencyHz / std::max(1.0, lowWindowCycles));
    const double highWidthHz = std::max(1.0, centerFrequencyHz / std::max(1.0, highWindowCycles));
    const double centerErb = erbRate(centerFrequencyHz);
    const double lowWindowErb = std::max(1.0e-6, centerErb - erbRate(std::max(1.0, centerFrequencyHz - lowWidthHz)));
    const double highWindowErb = std::max(1.0e-6, erbRate(centerFrequencyHz + highWidthHz) - centerErb);
    const double lowSigma = std::max(1.0e-6, lowWindowErb / kErbWindowEdgeSigma);
    const double highSigma = std::max(1.0e-6, highWindowErb / kErbWindowEdgeSigma);

    double totalWeight = 0.0;
    double totalPower = 0.0;
    for (size_t i = 0; i < frequencyAxisHz.size() && i < sourceDb.size(); ++i) {
        const double deltaErb = erbRate(frequencyAxisHz[i]) - centerErb;
        const double sigma = deltaErb < 0.0 ? lowSigma : highSigma;
        const double windowErb = deltaErb < 0.0 ? lowWindowErb : highWindowErb;
        if (std::abs(deltaErb) > windowErb) {
            continue;
        }

        const double normalized = deltaErb / sigma;
        const double weight = std::exp(-0.5 * normalized * normalized);
        totalWeight += weight;
        totalPower += weight * powerFromAmplitudeDb(sourceDb[i]);
    }

    if (totalWeight <= 1.0e-9) {
        return sourceDb[centerIndex];
    }
    return amplitudeDbFromPower(totalPower / totalWeight);
}

std::vector<double> smoothChannel(const std::vector<double>& frequencyAxisHz,
                                  const std::vector<double>& sourceDb,
                                  double lowWindowCycles,
                                  double highWindowCycles) {
    std::vector<double> smoothed;
    smoothed.reserve(std::min(frequencyAxisHz.size(), sourceDb.size()));
    for (size_t i = 0; i < frequencyAxisHz.size() && i < sourceDb.size(); ++i) {
        smoothed.push_back(smoothedValueAt(frequencyAxisHz,
                                           sourceDb,
                                           i,
                                           lowWindowCycles,
                                           highWindowCycles));
    }
    return smoothed;
}

std::vector<double> smoothChannelTwelfthOctaveSlidingWindow(const std::vector<double>& frequencyAxisHz,
                                                            const std::vector<double>& sourceDb,
                                                            double octaveDenominator) {
    std::vector<double> smoothed;
    smoothed.reserve(std::min(frequencyAxisHz.size(), sourceDb.size()));
    constexpr double kOctaveToRatio = 0.6931471805599453;
    for (size_t i = 0; i < frequencyAxisHz.size() && i < sourceDb.size(); ++i) {
        const double centerFrequencyHz = std::max(1.0, frequencyAxisHz[i]);
        const double halfWindowOctaves = 0.5 / std::max(1.0, octaveDenominator);
        const double ratio = std::exp(kOctaveToRatio * halfWindowOctaves);
        const double lowFrequencyHz = centerFrequencyHz / ratio;
        const double highFrequencyHz = centerFrequencyHz * ratio;
        smoothed.push_back(amplitudeDbFromPower(averagePowerBand(frequencyAxisHz,
                                                                 sourceDb,
                                                                 lowFrequencyHz,
                                                                 highFrequencyHz,
                                                                 i)));
    }
    return smoothed;
}

double interpolatedValueAt(const std::vector<double>& frequencyAxisHz,
                           const std::vector<double>& sourceDb,
                           double cutoffHz) {
    if (frequencyAxisHz.empty() || sourceDb.empty()) {
        return -90.0;
    }

    for (size_t i = 0; i < frequencyAxisHz.size() && i < sourceDb.size(); ++i) {
        if (frequencyAxisHz[i] >= cutoffHz) {
            if (i == 0) {
                return sourceDb.front();
            }

            const double lowFrequencyHz = frequencyAxisHz[i - 1];
            const double highFrequencyHz = frequencyAxisHz[i];
            const double rangeHz = std::max(1.0, highFrequencyHz - lowFrequencyHz);
            const double blend = clampValue((cutoffHz - lowFrequencyHz) / rangeHz, 0.0, 1.0);
            return sourceDb[i - 1] + ((sourceDb[i] - sourceDb[i - 1]) * blend);
        }
    }

    return sourceDb.back();
}

void flattenHighFrequencyTail(const std::vector<double>& frequencyAxisHz,
                              double cutoffHz,
                              std::vector<double>& sourceDb) {
    if (frequencyAxisHz.empty() || sourceDb.empty()) {
        return;
    }

    const double tailValueDb = interpolatedValueAt(frequencyAxisHz, sourceDb, cutoffHz);
    for (size_t i = 0; i < frequencyAxisHz.size() && i < sourceDb.size(); ++i) {
        if (frequencyAxisHz[i] > cutoffHz) {
            sourceDb[i] = tailValueDb;
        }
    }
}

}  // namespace

void normalizeResponseSmoothingSettings(ResponseSmoothingSettings& settings) {
    settings.psychoacousticModel = canonicalModelName(settings.psychoacousticModel);
    settings.resolutionPercent = clampValue(settings.resolutionPercent, 0, 100);
    settings.lowFrequencyWindowCycles = clampValue(settings.lowFrequencyWindowCycles, 1.0, 120.0);
    settings.highFrequencyWindowCycles = clampValue(settings.highFrequencyWindowCycles, 1.0, 120.0);
    settings.highFrequencySlopeCutoffHz = clampValue(settings.highFrequencySlopeCutoffHz, 1000.0, 48000.0);
}

double smoothingResolutionFactor(const ResponseSmoothingSettings& settings) {
    return resolutionFactorFromPercent(settings.resolutionPercent);
}

int effectiveLowWindowCycles(const ResponseSmoothingSettings& settings) {
    return std::max(1, static_cast<int>(std::lround(settings.lowFrequencyWindowCycles * smoothingResolutionFactor(settings))));
}

int effectiveHighWindowCycles(const ResponseSmoothingSettings& settings) {
    return std::max(1, static_cast<int>(std::lround(settings.highFrequencyWindowCycles * smoothingResolutionFactor(settings))));
}

int effectiveSlidingOctaveDenominator(const ResponseSmoothingSettings& settings) {
    return std::max(1, static_cast<int>(std::lround(12.0 * smoothingResolutionFactor(settings))));
}

SmoothedResponse buildSmoothedResponse(const MeasurementResult& result,
                                       const ResponseSmoothingSettings& settings) {
    SmoothedResponse smoothedResponse;
    if (result.frequencyAxisHz.empty() ||
        result.leftChannelDb.size() != result.frequencyAxisHz.size() ||
        result.rightChannelDb.size() != result.frequencyAxisHz.size()) {
        return smoothedResponse;
    }

    ResponseSmoothingSettings normalizedSettings = settings;
    normalizeResponseSmoothingSettings(normalizedSettings);

    smoothedResponse.frequencyAxisHz = result.frequencyAxisHz;
    if (normalizedSettings.psychoacousticModel == kOctaveSlidingWindowModelName) {
        const double octaveDenominator = static_cast<double>(effectiveSlidingOctaveDenominator(normalizedSettings));
        smoothedResponse.leftChannelDb = smoothChannelTwelfthOctaveSlidingWindow(result.frequencyAxisHz,
                                                                                 result.leftChannelDb,
                                                                                 octaveDenominator);
        smoothedResponse.rightChannelDb = smoothChannelTwelfthOctaveSlidingWindow(result.frequencyAxisHz,
                                                                                  result.rightChannelDb,
                                                                                  octaveDenominator);
    } else {
        smoothedResponse.leftChannelDb = smoothChannel(result.frequencyAxisHz,
                                                       result.leftChannelDb,
                                                       static_cast<double>(effectiveLowWindowCycles(normalizedSettings)),
                                                       static_cast<double>(effectiveHighWindowCycles(normalizedSettings)));
        smoothedResponse.rightChannelDb = smoothChannel(result.frequencyAxisHz,
                                                        result.rightChannelDb,
                                                        static_cast<double>(effectiveLowWindowCycles(normalizedSettings)),
                                                        static_cast<double>(effectiveHighWindowCycles(normalizedSettings)));
    }
    flattenHighFrequencyTail(smoothedResponse.frequencyAxisHz,
                             normalizedSettings.highFrequencySlopeCutoffHz,
                             smoothedResponse.leftChannelDb);
    flattenHighFrequencyTail(smoothedResponse.frequencyAxisHz,
                             normalizedSettings.highFrequencySlopeCutoffHz,
                             smoothedResponse.rightChannelDb);
    return smoothedResponse;
}

}  // namespace wolfie::measurement
