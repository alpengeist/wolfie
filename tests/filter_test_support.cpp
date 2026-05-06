#include "filter_test_support.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <numbers>

#include "measurement/dsp_utils.h"

namespace wolfie::tests {

std::vector<double> buildLogAxis(double minFrequencyHz, double maxFrequencyHz, int pointCount) {
    std::vector<double> axis;
    axis.reserve(static_cast<size_t>(pointCount));
    const double logMin = std::log10(std::max(minFrequencyHz, 1.0));
    const double logMax = std::log10(std::max(maxFrequencyHz, minFrequencyHz + 1.0));
    for (int index = 0; index < pointCount; ++index) {
        const double t = pointCount == 1 ? 0.0 : static_cast<double>(index) / static_cast<double>(pointCount - 1);
        axis.push_back(std::pow(10.0, logMin + ((logMax - logMin) * t)));
    }
    return axis;
}

wolfie::SmoothedResponse buildSyntheticResponse() {
    wolfie::SmoothedResponse response;
    response.frequencyAxisHz = buildLogAxis(20.0, 20000.0, 512);
    response.leftChannelDb.reserve(response.frequencyAxisHz.size());
    response.rightChannelDb.reserve(response.frequencyAxisHz.size());
    for (const double frequencyHz : response.frequencyAxisHz) {
        const double logRatio = std::log10(frequencyHz / 1000.0);
        response.leftChannelDb.push_back((-4.0 * logRatio) + (2.5 * std::exp(-std::pow((frequencyHz - 75.0) / 55.0, 2.0))));
        response.rightChannelDb.push_back((-3.0 * logRatio) - (2.0 * std::exp(-std::pow((frequencyHz - 2800.0) / 1200.0, 2.0))));
    }
    return response;
}

wolfie::SmoothedResponse buildFlatResponse(double levelDb) {
    wolfie::SmoothedResponse response;
    response.frequencyAxisHz = buildLogAxis(20.0, 20000.0, 512);
    response.leftChannelDb.assign(response.frequencyAxisHz.size(), levelDb);
    response.rightChannelDb.assign(response.frequencyAxisHz.size(), levelDb);
    return response;
}

wolfie::SmoothedResponse buildLowFrequencyRollOffResponse() {
    wolfie::SmoothedResponse response;
    response.frequencyAxisHz = buildLogAxis(20.0, 20000.0, 512);
    response.leftChannelDb.reserve(response.frequencyAxisHz.size());
    response.rightChannelDb.reserve(response.frequencyAxisHz.size());
    const double logLow = std::log10(20.0);
    const double logHigh = std::log10(120.0);
    for (const double frequencyHz : response.frequencyAxisHz) {
        const double clampedFrequencyHz = std::clamp(frequencyHz, 20.0, 120.0);
        const double lowTiltT = (logHigh - std::log10(clampedFrequencyHz)) / (logHigh - logLow);
        const double lowTiltDb = -14.0 * std::max(lowTiltT, 0.0);
        const double resonanceDb =
            -6.0 * std::exp(-std::pow((std::log10(std::max(frequencyHz, 1.0)) - std::log10(34.0)) / 0.09, 2.0));
        const double responseDb = lowTiltDb + resonanceDb;
        response.leftChannelDb.push_back(responseDb);
        response.rightChannelDb.push_back(responseDb);
    }
    return response;
}

double wrapDegrees(double phaseDegrees) {
    double wrapped = std::remainder(phaseDegrees, 360.0);
    if (wrapped <= -180.0) {
        wrapped += 360.0;
    } else if (wrapped > 180.0) {
        wrapped -= 360.0;
    }
    return wrapped;
}

wolfie::MeasurementValueSet buildImpulseValueSet(double leadingTimeSeconds) {
    wolfie::MeasurementValueSet valueSet;
    valueSet.key = "measurement.raw_impulse_response";
    valueSet.xQuantity = "time";
    valueSet.xUnit = "s";
    valueSet.yQuantity = "amplitude";
    valueSet.yUnit = "linear";
    valueSet.xValues = {
        leadingTimeSeconds,
        leadingTimeSeconds + 1.0e-4,
        leadingTimeSeconds + 2.0e-4
    };
    valueSet.leftValues = {0.0, 1.0, 0.0};
    valueSet.rightValues = {0.0, 1.0, 0.0};
    return valueSet;
}

wolfie::MeasurementValueSet buildWrappedPhaseSpectrum(const std::vector<double>& frequencyAxisHz,
                                                      const std::string& key,
                                                      double delaySeconds,
                                                      double leftExcessScale,
                                                      double rightExcessScale) {
    wolfie::MeasurementValueSet valueSet;
    valueSet.key = key;
    valueSet.xQuantity = "frequency";
    valueSet.xUnit = "Hz";
    valueSet.yQuantity = "phase";
    valueSet.yUnit = "deg";
    valueSet.xValues = frequencyAxisHz;
    valueSet.leftValues.reserve(frequencyAxisHz.size());
    valueSet.rightValues.reserve(frequencyAxisHz.size());
    for (const double frequencyHz : frequencyAxisHz) {
        const double linearDelayDegrees = -360.0 * frequencyHz * delaySeconds;
        const double safeFrequencyHz = std::max(frequencyHz, 1.0);
        const double excessShape = std::exp(-std::pow((std::log10(safeFrequencyHz) - std::log10(70.0)) / 0.22, 2.0)) *
                                   (1.0 - std::exp(-safeFrequencyHz / 22.0));
        valueSet.leftValues.push_back(wrapDegrees(linearDelayDegrees + (leftExcessScale * 75.0 * excessShape)));
        valueSet.rightValues.push_back(wrapDegrees(linearDelayDegrees + (rightExcessScale * 75.0 * excessShape)));
    }
    return valueSet;
}

wolfie::MeasurementValueSet buildFlatMagnitudeSpectrum(const std::vector<double>& frequencyAxisHz,
                                                       const std::string& key,
                                                       double leftLevelDb,
                                                       double rightLevelDb) {
    wolfie::MeasurementValueSet valueSet;
    valueSet.key = key;
    valueSet.xQuantity = "frequency";
    valueSet.xUnit = "Hz";
    valueSet.yQuantity = "level";
    valueSet.yUnit = "dB";
    valueSet.xValues = frequencyAxisHz;
    valueSet.leftValues.assign(frequencyAxisHz.size(), leftLevelDb);
    valueSet.rightValues.assign(frequencyAxisHz.size(), rightLevelDb);
    return valueSet;
}

std::vector<double> buildLinearAxis(double maxFrequencyHz, int pointCount) {
    std::vector<double> axis;
    axis.reserve(static_cast<size_t>(pointCount));
    for (int index = 0; index < pointCount; ++index) {
        const double t = pointCount <= 1 ? 0.0 : static_cast<double>(index) / static_cast<double>(pointCount - 1);
        axis.push_back(maxFrequencyHz * t);
    }
    return axis;
}

wolfie::MeasurementResult buildPhaseMeasurement(int sampleRate,
                                                double delaySeconds,
                                                double leftExcessScale,
                                                double rightExcessScale) {
    wolfie::MeasurementResult result;
    const std::vector<double> phaseAxisHz = buildLinearAxis(static_cast<double>(sampleRate) * 0.5, 4097);
    result.valueSets.push_back(buildImpulseValueSet(-delaySeconds));
    result.valueSets.push_back(buildImpulseValueSet(-delaySeconds));
    result.valueSets.back().key = "measurement.room_impulse_response";
    result.valueSets.push_back(buildImpulseValueSet(-delaySeconds));
    result.valueSets.back().key = "measurement.direct_impulse_response";
    result.valueSets.push_back(buildFlatMagnitudeSpectrum(phaseAxisHz, "measurement.raw_magnitude_spectrum"));
    result.valueSets.push_back(buildWrappedPhaseSpectrum(phaseAxisHz,
                                                        "measurement.raw_phase_spectrum",
                                                        delaySeconds,
                                                        leftExcessScale,
                                                        rightExcessScale));
    result.valueSets.push_back(buildFlatMagnitudeSpectrum(phaseAxisHz, "measurement.room_magnitude_spectrum"));
    result.valueSets.push_back(buildWrappedPhaseSpectrum(phaseAxisHz,
                                                        "measurement.room_phase_spectrum",
                                                        delaySeconds,
                                                        leftExcessScale,
                                                        rightExcessScale));
    result.valueSets.push_back(buildFlatMagnitudeSpectrum(phaseAxisHz, "measurement.direct_magnitude_spectrum"));
    result.valueSets.push_back(buildWrappedPhaseSpectrum(phaseAxisHz,
                                                        "measurement.direct_phase_spectrum",
                                                        delaySeconds,
                                                        leftExcessScale,
                                                        rightExcessScale));
    return result;
}

wolfie::MeasurementResult buildPhaseMeasurementWithSourceAvailability(int sampleRate,
                                                                      double delaySeconds,
                                                                      bool includeDirect,
                                                                      bool includeRoom,
                                                                      bool includeRaw,
                                                                      double directLeftExcessScale,
                                                                      double directRightExcessScale,
                                                                      double roomLeftExcessScale,
                                                                      double roomRightExcessScale,
                                                                      double rawLeftExcessScale,
                                                                      double rawRightExcessScale) {
    wolfie::MeasurementResult result;
    const std::vector<double> phaseAxisHz = buildLinearAxis(static_cast<double>(sampleRate) * 0.5, 4097);
    result.valueSets.push_back(buildImpulseValueSet(-delaySeconds));
    if (includeRoom) {
        result.valueSets.push_back(buildImpulseValueSet(-delaySeconds));
        result.valueSets.back().key = "measurement.room_impulse_response";
    }
    if (includeDirect) {
        result.valueSets.push_back(buildImpulseValueSet(-delaySeconds));
        result.valueSets.back().key = "measurement.direct_impulse_response";
    }
    if (includeRaw) {
        result.valueSets.push_back(buildFlatMagnitudeSpectrum(phaseAxisHz, "measurement.raw_magnitude_spectrum"));
        result.valueSets.push_back(buildWrappedPhaseSpectrum(phaseAxisHz,
                                                            "measurement.raw_phase_spectrum",
                                                            delaySeconds,
                                                            rawLeftExcessScale,
                                                            rawRightExcessScale));
    }
    if (includeRoom) {
        result.valueSets.push_back(buildFlatMagnitudeSpectrum(phaseAxisHz, "measurement.room_magnitude_spectrum"));
        result.valueSets.push_back(buildWrappedPhaseSpectrum(phaseAxisHz,
                                                            "measurement.room_phase_spectrum",
                                                            delaySeconds,
                                                            roomLeftExcessScale,
                                                            roomRightExcessScale));
    }
    if (includeDirect) {
        result.valueSets.push_back(buildFlatMagnitudeSpectrum(phaseAxisHz, "measurement.direct_magnitude_spectrum"));
        result.valueSets.push_back(buildWrappedPhaseSpectrum(phaseAxisHz,
                                                            "measurement.direct_phase_spectrum",
                                                            delaySeconds,
                                                            directLeftExcessScale,
                                                            directRightExcessScale));
    }
    return result;
}

wolfie::MeasurementResult buildImpulsePhaseMeasurement(int sampleRate,
                                                       double delaySeconds,
                                                       int lateTapOffsetSamples,
                                                       double lateTapGain) {
    wolfie::MeasurementResult result;
    const size_t fftSize = 8192;
    std::vector<std::complex<double>> spectrum(fftSize, {0.0, 0.0});
    spectrum[0] = {1.0, 0.0};
    if (lateTapOffsetSamples > 0 && static_cast<size_t>(lateTapOffsetSamples) < spectrum.size()) {
        spectrum[static_cast<size_t>(lateTapOffsetSamples)] = {lateTapGain, 0.0};
    }
    wolfie::measurement::fft(spectrum, false);

    wolfie::MeasurementValueSet magnitude;
    magnitude.key = "measurement.room_magnitude_spectrum";
    magnitude.xQuantity = "frequency";
    magnitude.xUnit = "Hz";
    magnitude.yQuantity = "level";
    magnitude.yUnit = "dB";

    wolfie::MeasurementValueSet phase;
    phase.key = "measurement.room_phase_spectrum";
    phase.xQuantity = "frequency";
    phase.xUnit = "Hz";
    phase.yQuantity = "phase";
    phase.yUnit = "deg";

    const size_t positiveBinCount = (fftSize / 2) + 1;
    magnitude.xValues.reserve(positiveBinCount);
    magnitude.leftValues.reserve(positiveBinCount);
    magnitude.rightValues.reserve(positiveBinCount);
    phase.xValues.reserve(positiveBinCount);
    phase.leftValues.reserve(positiveBinCount);
    phase.rightValues.reserve(positiveBinCount);
    for (size_t index = 0; index < positiveBinCount; ++index) {
        const double frequencyHz = static_cast<double>(sampleRate) * static_cast<double>(index) /
                                   static_cast<double>(fftSize);
        const double magnitudeDb = 20.0 * std::log10(std::max(std::abs(spectrum[index]), 1.0e-9));
        const double phaseDegrees = wrapDegrees(std::arg(spectrum[index]) * 180.0 / std::numbers::pi_v<double>);
        magnitude.xValues.push_back(frequencyHz);
        magnitude.leftValues.push_back(magnitudeDb);
        magnitude.rightValues.push_back(magnitudeDb);
        phase.xValues.push_back(frequencyHz);
        phase.leftValues.push_back(phaseDegrees);
        phase.rightValues.push_back(phaseDegrees);
    }

    result.valueSets.push_back(buildImpulseValueSet(-delaySeconds));
    result.valueSets.back().key = "measurement.raw_impulse_response";
    result.valueSets.push_back(buildImpulseValueSet(-delaySeconds));
    result.valueSets.back().key = "measurement.room_impulse_response";
    result.valueSets.push_back(buildImpulseValueSet(-delaySeconds));
    result.valueSets.back().key = "measurement.direct_impulse_response";
    result.valueSets.push_back(std::move(magnitude));
    result.valueSets.push_back(std::move(phase));
    return result;
}

std::vector<char> readFileBytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }

    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size <= 0) {
        return {};
    }

    in.seekg(0, std::ios::beg);
    std::vector<char> bytes(static_cast<size_t>(size));
    in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!in) {
        return {};
    }
    return bytes;
}

double bandMeanAbs(const std::vector<double>& frequencyAxisHz,
                   const std::vector<double>& values,
                   double minFrequencyHz,
                   double maxFrequencyHz) {
    const size_t count = std::min(frequencyAxisHz.size(), values.size());
    double sum = 0.0;
    size_t used = 0;
    for (size_t index = 0; index < count; ++index) {
        if (!std::isfinite(values[index])) {
            continue;
        }
        if (frequencyAxisHz[index] < minFrequencyHz || frequencyAxisHz[index] > maxFrequencyHz) {
            continue;
        }
        sum += std::abs(values[index]);
        ++used;
    }
    if (used == 0) {
        return std::numeric_limits<double>::infinity();
    }
    return sum / static_cast<double>(used);
}

double bandMeanAbsDelta(const std::vector<double>& frequencyAxisHz,
                        const std::vector<double>& leftValues,
                        const std::vector<double>& rightValues,
                        double minFrequencyHz,
                        double maxFrequencyHz) {
    const size_t count = std::min({frequencyAxisHz.size(), leftValues.size(), rightValues.size()});
    double sum = 0.0;
    size_t used = 0;
    for (size_t index = 0; index < count; ++index) {
        if (!std::isfinite(leftValues[index]) || !std::isfinite(rightValues[index])) {
            continue;
        }
        if (frequencyAxisHz[index] < minFrequencyHz || frequencyAxisHz[index] > maxFrequencyHz) {
            continue;
        }
        sum += std::abs(leftValues[index] - rightValues[index]);
        ++used;
    }
    if (used == 0) {
        return std::numeric_limits<double>::infinity();
    }
    return sum / static_cast<double>(used);
}

double maxAdjacentAbsDelta(const std::vector<double>& values) {
    if (values.size() < 2) {
        return 0.0;
    }

    double maxDelta = 0.0;
    for (size_t index = 1; index < values.size(); ++index) {
        if (!std::isfinite(values[index - 1]) || !std::isfinite(values[index])) {
            continue;
        }
        maxDelta = std::max(maxDelta, std::abs(values[index] - values[index - 1]));
    }
    return maxDelta;
}

bool processLogContains(const wolfie::FilterDesignResult& result, const std::string& needle) {
    for (const std::string& entry : result.processLog) {
        if (entry.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

}  // namespace wolfie::tests
