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

int clampOctaveDenominator(int denominator) {
    int clamped = clampValue(denominator, 2, 24);
    if ((clamped % 2) != 0) {
        clamped += (clamped == 24) ? -1 : 1;
    }
    return clamped;
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

std::vector<double> buildPowerSeries(const std::vector<double>& sourceDb, size_t count) {
    std::vector<double> power;
    power.reserve(count);
    for (size_t index = 0; index < count; ++index) {
        power.push_back(powerFromAmplitudeDb(sourceDb[index]));
    }
    return power;
}

std::vector<double> buildPrefixSums(const std::vector<double>& values) {
    std::vector<double> prefixSums(values.size() + 1, 0.0);
    for (size_t index = 0; index < values.size(); ++index) {
        prefixSums[index + 1] = prefixSums[index] + values[index];
    }
    return prefixSums;
}

std::vector<double> buildErbAxis(const std::vector<double>& frequencyAxisHz, size_t count) {
    std::vector<double> erbAxis;
    erbAxis.reserve(count);
    for (size_t index = 0; index < count; ++index) {
        erbAxis.push_back(erbRate(frequencyAxisHz[index]));
    }
    return erbAxis;
}

double averagePowerBand(const std::vector<double>& frequencyAxisHz,
                        const std::vector<double>& sourceDb,
                        const std::vector<double>& powerPrefixSums,
                        double lowFrequencyHz,
                        double highFrequencyHz,
                        size_t centerIndex,
                        size_t count) {
    const auto bandBegin = std::lower_bound(frequencyAxisHz.begin(),
                                            frequencyAxisHz.begin() + static_cast<std::ptrdiff_t>(count),
                                            lowFrequencyHz);
    const auto bandEnd = std::upper_bound(frequencyAxisHz.begin(),
                                          frequencyAxisHz.begin() + static_cast<std::ptrdiff_t>(count),
                                          highFrequencyHz);
    const size_t beginIndex = static_cast<size_t>(std::distance(frequencyAxisHz.begin(), bandBegin));
    const size_t endIndex = static_cast<size_t>(std::distance(frequencyAxisHz.begin(), bandEnd));
    if (beginIndex >= endIndex) {
        return powerFromAmplitudeDb(sourceDb[centerIndex]);
    }

    const double totalPower = powerPrefixSums[endIndex] - powerPrefixSums[beginIndex];
    return totalPower / static_cast<double>(endIndex - beginIndex);
}

double smoothedValueAt(const std::vector<double>& frequencyAxisHz,
                       const std::vector<double>& sourceDb,
                       const std::vector<double>& erbAxis,
                       const std::vector<double>& power,
                       size_t count,
                       size_t centerIndex,
                       double lowWindowCycles,
                       double highWindowCycles) {
    const double centerFrequencyHz = std::max(1.0, frequencyAxisHz[centerIndex]);
    const double lowWidthHz = std::max(1.0, centerFrequencyHz / std::max(1.0, lowWindowCycles));
    const double highWidthHz = std::max(1.0, centerFrequencyHz / std::max(1.0, highWindowCycles));
    const double centerErb = erbAxis[centerIndex];
    const double lowWindowErb = std::max(1.0e-6, centerErb - erbRate(std::max(1.0, centerFrequencyHz - lowWidthHz)));
    const double highWindowErb = std::max(1.0e-6, erbRate(centerFrequencyHz + highWidthHz) - centerErb);
    const double lowSigma = std::max(1.0e-6, lowWindowErb / kErbWindowEdgeSigma);
    const double highSigma = std::max(1.0e-6, highWindowErb / kErbWindowEdgeSigma);
    const double leftErbLimit = centerErb - lowWindowErb;
    const double rightErbLimit = centerErb + highWindowErb;

    const auto windowBegin = std::lower_bound(erbAxis.begin(),
                                              erbAxis.begin() + static_cast<std::ptrdiff_t>(count),
                                              leftErbLimit);
    const auto windowEnd = std::upper_bound(erbAxis.begin(),
                                            erbAxis.begin() + static_cast<std::ptrdiff_t>(count),
                                            rightErbLimit);
    const size_t beginIndex = static_cast<size_t>(std::distance(erbAxis.begin(), windowBegin));
    const size_t endIndex = static_cast<size_t>(std::distance(erbAxis.begin(), windowEnd));

    double totalWeight = 0.0;
    double totalPower = 0.0;
    for (size_t i = beginIndex; i < endIndex; ++i) {
        const double deltaErb = erbAxis[i] - centerErb;
        const double sigma = deltaErb < 0.0 ? lowSigma : highSigma;
        const double windowErb = deltaErb < 0.0 ? lowWindowErb : highWindowErb;
        if (std::abs(deltaErb) > windowErb) {
            continue;
        }

        const double normalized = deltaErb / sigma;
        const double weight = std::exp(-0.5 * normalized * normalized);
        totalWeight += weight;
        totalPower += weight * power[i];
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
    const size_t count = std::min(frequencyAxisHz.size(), sourceDb.size());
    std::vector<double> smoothed;
    smoothed.reserve(count);
    if (count == 0) {
        return smoothed;
    }

    const std::vector<double> erbAxis = buildErbAxis(frequencyAxisHz, count);
    const std::vector<double> power = buildPowerSeries(sourceDb, count);
    for (size_t i = 0; i < count; ++i) {
        smoothed.push_back(smoothedValueAt(frequencyAxisHz,
                                           sourceDb,
                                           erbAxis,
                                           power,
                                           count,
                                           i,
                                           lowWindowCycles,
                                           highWindowCycles));
    }
    return smoothed;
}

std::vector<double> smoothChannelTwelfthOctaveSlidingWindow(const std::vector<double>& frequencyAxisHz,
                                                            const std::vector<double>& sourceDb,
                                                            double octaveDenominator) {
    const size_t count = std::min(frequencyAxisHz.size(), sourceDb.size());
    std::vector<double> smoothed;
    smoothed.reserve(count);
    if (count == 0) {
        return smoothed;
    }

    const std::vector<double> power = buildPowerSeries(sourceDb, count);
    const std::vector<double> powerPrefixSums = buildPrefixSums(power);
    constexpr double kOctaveToRatio = 0.6931471805599453;
    for (size_t i = 0; i < count; ++i) {
        const double centerFrequencyHz = std::max(1.0, frequencyAxisHz[i]);
        const double halfWindowOctaves = 0.5 / std::max(1.0, octaveDenominator);
        const double ratio = std::exp(kOctaveToRatio * halfWindowOctaves);
        const double lowFrequencyHz = centerFrequencyHz / ratio;
        const double highFrequencyHz = centerFrequencyHz * ratio;
        smoothed.push_back(amplitudeDbFromPower(averagePowerBand(frequencyAxisHz,
                                                                 sourceDb,
                                                                 powerPrefixSums,
                                                                 lowFrequencyHz,
                                                                 highFrequencyHz,
                                                                 i,
                                                                 count)));
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
    settings.resolutionPercent = clampOctaveDenominator(settings.resolutionPercent);
    settings.lowFrequencyWindowCycles = clampValue(settings.lowFrequencyWindowCycles, 5.0, 20.0);
    settings.highFrequencyWindowCycles = clampValue(settings.highFrequencyWindowCycles, 5.0, 20.0);
    settings.highFrequencySlopeCutoffHz = clampValue(settings.highFrequencySlopeCutoffHz, 1000.0, 48000.0);
}

double smoothingResolutionFactor(const ResponseSmoothingSettings& settings) {
    ResponseSmoothingSettings normalizedSettings = settings;
    normalizeResponseSmoothingSettings(normalizedSettings);
    if (normalizedSettings.psychoacousticModel == kOctaveSlidingWindowModelName) {
        return static_cast<double>(normalizedSettings.resolutionPercent) / 12.0;
    }
    return (static_cast<double>(effectiveLowWindowCycles(normalizedSettings)) +
            static_cast<double>(effectiveHighWindowCycles(normalizedSettings))) / 30.0;
}

int effectiveLowWindowCycles(const ResponseSmoothingSettings& settings) {
    return clampValue(static_cast<int>(std::lround(settings.lowFrequencyWindowCycles)), 5, 20);
}

int effectiveHighWindowCycles(const ResponseSmoothingSettings& settings) {
    return clampValue(static_cast<int>(std::lround(settings.highFrequencyWindowCycles)), 5, 20);
}

int effectiveSlidingOctaveDenominator(const ResponseSmoothingSettings& settings) {
    return clampOctaveDenominator(settings.resolutionPercent);
}

std::vector<double> smoothMagnitudeSeries(const std::vector<double>& frequencyAxisHz,
                                          const std::vector<double>& sourceDb,
                                          const ResponseSmoothingSettings& settings) {
    ResponseSmoothingSettings normalizedSettings = settings;
    normalizeResponseSmoothingSettings(normalizedSettings);

    std::vector<double> smoothed;
    if (normalizedSettings.psychoacousticModel == kOctaveSlidingWindowModelName) {
        const double octaveDenominator = static_cast<double>(effectiveSlidingOctaveDenominator(normalizedSettings));
        smoothed = smoothChannelTwelfthOctaveSlidingWindow(frequencyAxisHz,
                                                           sourceDb,
                                                           octaveDenominator);
    } else {
        smoothed = smoothChannel(frequencyAxisHz,
                                 sourceDb,
                                 static_cast<double>(effectiveLowWindowCycles(normalizedSettings)),
                                 static_cast<double>(effectiveHighWindowCycles(normalizedSettings)));
    }
    flattenHighFrequencyTail(frequencyAxisHz,
                             normalizedSettings.highFrequencySlopeCutoffHz,
                             smoothed);
    return smoothed;
}

SmoothedResponse buildSmoothedResponse(const MeasurementResult& result,
                                       const ResponseSmoothingSettings& settings) {
    SmoothedResponse smoothedResponse;
    const MeasurementValueSet* magnitudeResponse = result.magnitudeResponse();
    if (magnitudeResponse == nullptr || !magnitudeResponse->valid()) {
        return smoothedResponse;
    }

    ResponseSmoothingSettings normalizedSettings = settings;
    normalizeResponseSmoothingSettings(normalizedSettings);

    smoothedResponse.smoothingSettings = normalizedSettings;
    smoothedResponse.frequencyAxisHz = magnitudeResponse->xValues;
    smoothedResponse.leftChannelDb = smoothMagnitudeSeries(magnitudeResponse->xValues,
                                                           magnitudeResponse->leftValues,
                                                           normalizedSettings);
    smoothedResponse.rightChannelDb = smoothMagnitudeSeries(magnitudeResponse->xValues,
                                                            magnitudeResponse->rightValues,
                                                            normalizedSettings);
    return smoothedResponse;
}

}  // namespace wolfie::measurement
