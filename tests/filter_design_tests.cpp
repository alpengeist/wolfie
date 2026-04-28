#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <vector>

#include "core/models.h"
#include "measurement/filter_designer.h"
#include "measurement/filter_wav_export.h"
#include "measurement/target_curve_designer.h"

namespace {

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
                                                      double delaySeconds,
                                                      double leftExcessScale,
                                                      double rightExcessScale) {
    wolfie::MeasurementValueSet valueSet;
    valueSet.key = "measurement.raw_phase_spectrum";
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
                                                double leftExcessScale = 0.0,
                                                double rightExcessScale = 0.0) {
    wolfie::MeasurementResult result;
    const std::vector<double> phaseAxisHz = buildLinearAxis(static_cast<double>(sampleRate) * 0.5, 4097);
    result.valueSets.push_back(buildImpulseValueSet(-delaySeconds));
    result.valueSets.push_back(buildWrappedPhaseSpectrum(phaseAxisHz,
                                                        delaySeconds,
                                                        leftExcessScale,
                                                        rightExcessScale));
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

bool expectDesignedFilterLooksSane() {
    wolfie::MeasurementSettings measurement;
    measurement.sampleRate = 48000;
    measurement.startFrequencyHz = 20.0;
    measurement.endFrequencyHz = 20000.0;

    wolfie::TargetCurveSettings targetCurve;
    targetCurve.lowGainDb = 2.0;
    targetCurve.midGainDb = 0.0;
    targetCurve.highGainDb = -1.5;
    wolfie::measurement::normalizeTargetCurveSettings(targetCurve, 20.0, 20000.0);

    wolfie::FilterDesignSettings filterSettings;
    filterSettings.tapCount = 16384;
    filterSettings.maxBoostDb = 6.0;
    filterSettings.maxCutDb = 12.0;
    filterSettings.highCorrectionHz = 18000.0;

    const wolfie::SmoothedResponse response = buildSyntheticResponse();
    const wolfie::FilterDesignResult result =
        wolfie::measurement::designFilters(response, measurement, targetCurve, filterSettings);
    if (!result.valid) {
        std::cerr << "filter design did not produce a valid result\n";
        return false;
    }
    if (result.tapCount != 16384 || result.fftSize != 65536) {
        std::cerr << "filter design used unexpected tap or fft size\n";
        return false;
    }
    if (result.left.filterTaps.size() != static_cast<size_t>(result.tapCount) ||
        result.right.filterTaps.size() != static_cast<size_t>(result.tapCount)) {
        std::cerr << "filter design returned the wrong tap count\n";
        return false;
    }
    if (result.frequencyAxisHz.size() != static_cast<size_t>(filterSettings.displayPointCount)) {
        std::cerr << "filter design returned the wrong display resolution\n";
        return false;
    }

    const auto maxLeftCorrection = std::max_element(result.left.correctionCurveDb.begin(),
                                                    result.left.correctionCurveDb.end());
    const auto minLeftCorrection = std::min_element(result.left.correctionCurveDb.begin(),
                                                    result.left.correctionCurveDb.end());
    if (maxLeftCorrection == result.left.correctionCurveDb.end() ||
        *maxLeftCorrection > 6.2 || *minLeftCorrection < -12.2) {
        std::cerr << "filter design did not respect boost/cut limits\n";
        return false;
    }

    double leftError = 0.0;
    double rightError = 0.0;
    for (size_t index = 0; index < result.frequencyAxisHz.size(); ++index) {
        leftError += std::abs(result.left.correctedResponseDb[index] - result.targetCurveDb[index]);
        rightError += std::abs(result.right.correctedResponseDb[index] - result.targetCurveDb[index]);
    }
    leftError /= static_cast<double>(result.frequencyAxisHz.size());
    rightError /= static_cast<double>(result.frequencyAxisHz.size());
    if (leftError > 1.5 || rightError > 1.5) {
        std::cerr << "predicted corrected response is too far from target\n";
        return false;
    }

    return true;
}

double interpolateLogFrequency(const std::vector<double>& frequencyAxisHz,
                               const std::vector<double>& values,
                               double frequencyHz) {
    if (frequencyAxisHz.empty() || values.empty()) {
        return 0.0;
    }

    const size_t count = std::min(frequencyAxisHz.size(), values.size());
    if (count == 1 || frequencyHz <= frequencyAxisHz.front()) {
        return values.front();
    }
    if (frequencyHz >= frequencyAxisHz[count - 1]) {
        return values[count - 1];
    }

    const auto upper = std::lower_bound(frequencyAxisHz.begin(),
                                        frequencyAxisHz.begin() + static_cast<std::ptrdiff_t>(count),
                                        frequencyHz);
    const size_t upperIndex = static_cast<size_t>(std::distance(frequencyAxisHz.begin(), upper));
    const size_t lowerIndex = upperIndex - 1;
    const double x0 = std::log10(std::max(frequencyAxisHz[lowerIndex], 1.0));
    const double x1 = std::log10(std::max(frequencyAxisHz[upperIndex], 1.0));
    const double x = std::log10(std::max(frequencyHz, 1.0));
    const double y0 = values[lowerIndex];
    const double y1 = values[upperIndex];
    const double t = std::clamp((x - x0) / std::max(x1 - x0, 1.0e-9), 0.0, 1.0);
    return y0 + ((y1 - y0) * t);
}

bool expectExactTargetCurveEvaluationCapturesBellPeak() {
    wolfie::MeasurementSettings measurement;
    measurement.sampleRate = 48000;
    measurement.startFrequencyHz = 20.0;
    measurement.endFrequencyHz = 20000.0;

    wolfie::SmoothedResponse sparseResponse;
    sparseResponse.frequencyAxisHz = buildLogAxis(20.0, 20000.0, 16);
    sparseResponse.leftChannelDb.assign(sparseResponse.frequencyAxisHz.size(), 0.0);
    sparseResponse.rightChannelDb.assign(sparseResponse.frequencyAxisHz.size(), 0.0);

    wolfie::TargetCurveSettings targetCurve;
    targetCurve.eqBands = {wolfie::measurement::makeDefaultTargetEqBand(1000.0, 0)};
    targetCurve.eqBands.front().enabled = true;
    targetCurve.eqBands.front().gainDb = 9.0;
    targetCurve.eqBands.front().q = 6.0;
    wolfie::measurement::normalizeTargetCurveSettings(targetCurve, 20.0, 20000.0);

    const wolfie::measurement::TargetCurvePlotData plot =
        wolfie::measurement::buildTargetCurvePlotData(sparseResponse, measurement, targetCurve, 0);
    const double exactDb =
        wolfie::measurement::evaluateTargetCurveDbAtFrequency(measurement, targetCurve, plot.minFrequencyHz, plot.maxFrequencyHz, 1000.0);
    const double interpolatedDb = interpolateLogFrequency(plot.frequencyAxisHz, plot.targetCurveDb, 1000.0);
    if (exactDb < 8.5) {
        std::cerr << "exact target-curve evaluation did not preserve the bell peak\n";
        return false;
    }
    if (interpolatedDb > exactDb - 1.0) {
        std::cerr << "sparse plot interpolation unexpectedly matched the bell peak\n";
        return false;
    }

    return true;
}

bool expectTargetCurveAnchorsToMeasuredLevel() {
    wolfie::MeasurementSettings measurement;
    measurement.sampleRate = 48000;
    measurement.startFrequencyHz = 20.0;
    measurement.endFrequencyHz = 20000.0;

    wolfie::TargetCurveSettings targetCurve;
    wolfie::measurement::normalizeTargetCurveSettings(targetCurve, 20.0, 20000.0);

    wolfie::FilterDesignSettings filterSettings;
    filterSettings.tapCount = 16384;
    filterSettings.maxBoostDb = 6.0;
    filterSettings.maxCutDb = 12.0;

    const wolfie::SmoothedResponse response = buildFlatResponse(18.0);
    const wolfie::FilterDesignResult result =
        wolfie::measurement::designFilters(response, measurement, targetCurve, filterSettings);
    if (!result.valid) {
        std::cerr << "level-anchoring test did not produce a valid filter result\n";
        return false;
    }

    double correctionMeanAbs = 0.0;
    double targetMeanDb = 0.0;
    for (size_t index = 0; index < result.frequencyAxisHz.size(); ++index) {
        correctionMeanAbs += std::abs(result.left.correctionCurveDb[index]);
        targetMeanDb += result.targetCurveDb[index];
    }
    correctionMeanAbs /= static_cast<double>(std::max<size_t>(result.frequencyAxisHz.size(), 1));
    targetMeanDb /= static_cast<double>(std::max<size_t>(result.frequencyAxisHz.size(), 1));
    if (correctionMeanAbs > 0.25) {
        std::cerr << "flat offset response still produced a non-trivial correction\n";
        return false;
    }
    if (std::abs(targetMeanDb - 18.0) > 0.25) {
        std::cerr << "target curve was not anchored to the measured response level\n";
        return false;
    }

    return true;
}

bool expectMinimumPhaseInputNeedsNoExcessCorrection() {
    wolfie::MeasurementSettings measurement;
    measurement.sampleRate = 48000;
    measurement.startFrequencyHz = 20.0;
    measurement.endFrequencyHz = 20000.0;

    wolfie::TargetCurveSettings targetCurve;
    wolfie::measurement::normalizeTargetCurveSettings(targetCurve, 20.0, 20000.0);

    wolfie::FilterDesignSettings filterSettings;
    filterSettings.tapCount = 16384;
    filterSettings.maxBoostDb = 6.0;
    filterSettings.maxCutDb = 12.0;

    const wolfie::SmoothedResponse response = buildFlatResponse(0.0);
    const wolfie::MeasurementResult phaseMeasurement =
        buildPhaseMeasurement(measurement.sampleRate, 0.0);
    const wolfie::FilterDesignResult result =
        wolfie::measurement::designFilters(response,
                                           measurement,
                                           targetCurve,
                                           filterSettings,
                                           &phaseMeasurement);
    if (!result.valid) {
        std::cerr << "minimum-phase baseline did not produce a valid filter result\n";
        return false;
    }
    if (result.left.inputExcessPhaseDegrees.size() != result.frequencyAxisHz.size() ||
        result.right.inputExcessPhaseDegrees.size() != result.frequencyAxisHz.size()) {
        std::cerr << "minimum-phase baseline did not populate excess-phase diagnostics\n";
        return false;
    }

    const double leftBandMean = bandMeanAbs(result.frequencyAxisHz,
                                            result.left.inputExcessPhaseDegrees,
                                            20.0,
                                            300.0);
    const double rightBandMean = bandMeanAbs(result.frequencyAxisHz,
                                             result.right.inputExcessPhaseDegrees,
                                             20.0,
                                             300.0);
    if (leftBandMean > 2.0 || rightBandMean > 2.0) {
        std::cerr << "minimum-phase baseline reported unexpected excess phase (left="
                  << leftBandMean << ", right=" << rightBandMean << ")\n";
        return false;
    }

    const double predictedLeftBandMean = bandMeanAbs(result.frequencyAxisHz,
                                                     result.left.predictedExcessPhaseDegrees,
                                                     20.0,
                                                     300.0);
    const double predictedRightBandMean = bandMeanAbs(result.frequencyAxisHz,
                                                      result.right.predictedExcessPhaseDegrees,
                                                      20.0,
                                                      300.0);
    if (predictedLeftBandMean > 2.0 || predictedRightBandMean > 2.0) {
        std::cerr << "minimum-phase baseline predicted unexpected excess phase (left="
                  << predictedLeftBandMean << ", right=" << predictedRightBandMean << ")\n";
        return false;
    }

    return true;
}

bool expectBulkDelayIsNotTreatedAsExcessPhase() {
    wolfie::MeasurementSettings measurement;
    measurement.sampleRate = 48000;
    measurement.startFrequencyHz = 20.0;
    measurement.endFrequencyHz = 20000.0;

    wolfie::TargetCurveSettings targetCurve;
    wolfie::measurement::normalizeTargetCurveSettings(targetCurve, 20.0, 20000.0);

    wolfie::FilterDesignSettings filterSettings;
    filterSettings.tapCount = 16384;
    filterSettings.maxBoostDb = 6.0;
    filterSettings.maxCutDb = 12.0;

    const wolfie::SmoothedResponse response = buildFlatResponse(0.0);
    const wolfie::MeasurementResult phaseMeasurement =
        buildPhaseMeasurement(measurement.sampleRate, 0.0065);
    const wolfie::FilterDesignResult result =
        wolfie::measurement::designFilters(response,
                                           measurement,
                                           targetCurve,
                                           filterSettings,
                                           &phaseMeasurement);
    if (!result.valid) {
        std::cerr << "bulk-delay baseline did not produce a valid filter result\n";
        return false;
    }

    const double leftBandMean = bandMeanAbs(result.frequencyAxisHz,
                                            result.left.inputExcessPhaseDegrees,
                                            20.0,
                                            300.0);
    const double rightBandMean = bandMeanAbs(result.frequencyAxisHz,
                                             result.right.inputExcessPhaseDegrees,
                                             20.0,
                                             300.0);
    if (leftBandMean > 3.0 || rightBandMean > 3.0) {
        std::cerr << "bulk delay was misclassified as excess phase (left="
                  << leftBandMean << ", right=" << rightBandMean << ")\n";
        return false;
    }

    const double predictedLeftBandMean = bandMeanAbs(result.frequencyAxisHz,
                                                     result.left.predictedExcessPhaseDegrees,
                                                     20.0,
                                                     300.0);
    const double predictedRightBandMean = bandMeanAbs(result.frequencyAxisHz,
                                                      result.right.predictedExcessPhaseDegrees,
                                                      20.0,
                                                      300.0);
    if (predictedLeftBandMean > 3.0 || predictedRightBandMean > 3.0) {
        std::cerr << "bulk-delay baseline predicted excess phase after delay removal (left="
                  << predictedLeftBandMean << ", right=" << predictedRightBandMean << ")\n";
        return false;
    }

    return true;
}

bool expectExcessLfModeLeavesMinimumPhaseInputAlone() {
    wolfie::MeasurementSettings measurement;
    measurement.sampleRate = 48000;
    measurement.startFrequencyHz = 20.0;
    measurement.endFrequencyHz = 20000.0;

    wolfie::TargetCurveSettings targetCurve;
    wolfie::measurement::normalizeTargetCurveSettings(targetCurve, 20.0, 20000.0);

    wolfie::FilterDesignSettings filterSettings;
    filterSettings.tapCount = 16384;
    filterSettings.phaseMode = "excess-lf";

    const wolfie::SmoothedResponse response = buildFlatResponse(0.0);
    const wolfie::MeasurementResult phaseMeasurement =
        buildPhaseMeasurement(measurement.sampleRate, 0.0);
    const wolfie::FilterDesignResult result =
        wolfie::measurement::designFilters(response,
                                           measurement,
                                           targetCurve,
                                           filterSettings,
                                           &phaseMeasurement);
    if (!result.valid || result.phaseMode != "excess-lf") {
        std::cerr << "excess-lf minimum-phase baseline did not produce the expected mode\n";
        return false;
    }

    const double leftPredictedBandMean = bandMeanAbs(result.frequencyAxisHz,
                                                     result.left.predictedExcessPhaseDegrees,
                                                     20.0,
                                                     300.0);
    const double rightPredictedBandMean = bandMeanAbs(result.frequencyAxisHz,
                                                      result.right.predictedExcessPhaseDegrees,
                                                      20.0,
                                                      300.0);
    if (leftPredictedBandMean > 2.0 || rightPredictedBandMean > 2.0) {
        std::cerr << "excess-lf mode introduced excess phase on minimum-phase input (left="
                  << leftPredictedBandMean << ", right=" << rightPredictedBandMean << ")\n";
        return false;
    }

    const double magnitudeDeltaLeft = bandMeanAbsDelta(result.frequencyAxisHz,
                                                       response.leftChannelDb,
                                                       result.left.correctedResponseDb,
                                                       20.0,
                                                       20000.0);
    const double magnitudeDeltaRight = bandMeanAbsDelta(result.frequencyAxisHz,
                                                        response.rightChannelDb,
                                                        result.right.correctedResponseDb,
                                                        20.0,
                                                        20000.0);
    if (magnitudeDeltaLeft > 0.05 || magnitudeDeltaRight > 0.05) {
        std::cerr << "excess-lf mode changed magnitude on a phase-only baseline (left="
                  << magnitudeDeltaLeft << ", right=" << magnitudeDeltaRight << ")\n";
        return false;
    }

    return true;
}

bool expectExcessLfModeIgnoresBulkDelay() {
    wolfie::MeasurementSettings measurement;
    measurement.sampleRate = 48000;
    measurement.startFrequencyHz = 20.0;
    measurement.endFrequencyHz = 20000.0;

    wolfie::TargetCurveSettings targetCurve;
    wolfie::measurement::normalizeTargetCurveSettings(targetCurve, 20.0, 20000.0);

    wolfie::FilterDesignSettings filterSettings;
    filterSettings.tapCount = 16384;
    filterSettings.phaseMode = "excess-lf";

    const wolfie::SmoothedResponse response = buildFlatResponse(0.0);
    const wolfie::MeasurementResult phaseMeasurement =
        buildPhaseMeasurement(measurement.sampleRate, 0.0065);
    const wolfie::FilterDesignResult result =
        wolfie::measurement::designFilters(response,
                                           measurement,
                                           targetCurve,
                                           filterSettings,
                                           &phaseMeasurement);
    if (!result.valid) {
        std::cerr << "excess-lf bulk-delay baseline did not produce a valid filter result\n";
        return false;
    }

    const double leftPredictedBandMean = bandMeanAbs(result.frequencyAxisHz,
                                                     result.left.predictedExcessPhaseDegrees,
                                                     20.0,
                                                     300.0);
    const double rightPredictedBandMean = bandMeanAbs(result.frequencyAxisHz,
                                                      result.right.predictedExcessPhaseDegrees,
                                                      20.0,
                                                      300.0);
    if (leftPredictedBandMean > 3.0 || rightPredictedBandMean > 3.0) {
        std::cerr << "excess-lf mode reacted to pure bulk delay (left="
                  << leftPredictedBandMean << ", right=" << rightPredictedBandMean << ")\n";
        return false;
    }

    return true;
}

bool expectExcessLfModeReducesLowFrequencyExcessPhase() {
    wolfie::MeasurementSettings measurement;
    measurement.sampleRate = 48000;
    measurement.startFrequencyHz = 20.0;
    measurement.endFrequencyHz = 20000.0;

    wolfie::TargetCurveSettings targetCurve;
    wolfie::measurement::normalizeTargetCurveSettings(targetCurve, 20.0, 20000.0);

    wolfie::FilterDesignSettings filterSettings;
    filterSettings.tapCount = 16384;
    filterSettings.phaseMode = "excess-lf";

    const wolfie::SmoothedResponse response = buildFlatResponse(0.0);
    const wolfie::MeasurementResult phaseMeasurement =
        buildPhaseMeasurement(measurement.sampleRate, 0.0, 1.0, 0.0);
    const wolfie::FilterDesignResult result =
        wolfie::measurement::designFilters(response,
                                           measurement,
                                           targetCurve,
                                           filterSettings,
                                           &phaseMeasurement);
    if (!result.valid) {
        std::cerr << "excess-lf reduction case did not produce a valid filter result\n";
        return false;
    }

    const double leftInputBandMean = bandMeanAbs(result.frequencyAxisHz,
                                                 result.left.inputExcessPhaseDegrees,
                                                 20.0,
                                                 200.0);
    const double leftPredictedBandMean = bandMeanAbs(result.frequencyAxisHz,
                                                     result.left.predictedExcessPhaseDegrees,
                                                     20.0,
                                                     200.0);
    if (leftInputBandMean < 8.0) {
        std::cerr << "synthetic excess-phase fixture did not produce a meaningful LF phase error\n";
        return false;
    }
    if (leftPredictedBandMean > leftInputBandMean * 0.6) {
        std::cerr << "excess-lf mode did not materially reduce LF excess phase (before="
                  << leftInputBandMean << ", after=" << leftPredictedBandMean << ")\n";
        return false;
    }

    const double rightPredictedBandMean = bandMeanAbs(result.frequencyAxisHz,
                                                      result.right.predictedExcessPhaseDegrees,
                                                      20.0,
                                                      200.0);
    if (rightPredictedBandMean > 2.0) {
        std::cerr << "excess-lf mode changed the clean channel while correcting the left channel (right="
                  << rightPredictedBandMean << ")\n";
        return false;
    }

    return true;
}

bool expectExcessLfModeContainsCorrectionToLowFrequencies() {
    wolfie::MeasurementSettings measurement;
    measurement.sampleRate = 48000;
    measurement.startFrequencyHz = 20.0;
    measurement.endFrequencyHz = 20000.0;

    wolfie::TargetCurveSettings targetCurve;
    wolfie::measurement::normalizeTargetCurveSettings(targetCurve, 20.0, 20000.0);

    wolfie::FilterDesignSettings filterSettings;
    filterSettings.tapCount = 16384;
    filterSettings.phaseMode = "excess-lf";

    const wolfie::SmoothedResponse response = buildFlatResponse(0.0);
    const wolfie::MeasurementResult phaseMeasurement =
        buildPhaseMeasurement(measurement.sampleRate, 0.0, 1.0, 0.0);
    const wolfie::FilterDesignResult result =
        wolfie::measurement::designFilters(response,
                                           measurement,
                                           targetCurve,
                                           filterSettings,
                                           &phaseMeasurement);
    if (!result.valid) {
        std::cerr << "excess-lf containment case did not produce a valid filter result\n";
        return false;
    }

    const double highBandDelta = bandMeanAbsDelta(result.frequencyAxisHz,
                                                  result.left.inputExcessPhaseDegrees,
                                                  result.left.predictedExcessPhaseDegrees,
                                                  500.0,
                                                  5000.0);
    if (highBandDelta > 5.0) {
        std::cerr << "excess-lf mode changed too much phase out of band (" << highBandDelta << " deg)\n";
        return false;
    }

    const double leftMagnitudeDelta = bandMeanAbsDelta(result.frequencyAxisHz,
                                                       response.leftChannelDb,
                                                       result.left.correctedResponseDb,
                                                       20.0,
                                                       20000.0);
    const double rightMagnitudeDelta = bandMeanAbsDelta(result.frequencyAxisHz,
                                                        response.rightChannelDb,
                                                        result.right.correctedResponseDb,
                                                        20.0,
                                                        20000.0);
    if (leftMagnitudeDelta > 0.05 || rightMagnitudeDelta > 0.05) {
        std::cerr << "excess-lf mode changed magnitude while still in isolated phase preview (left="
                  << leftMagnitudeDelta << ", right=" << rightMagnitudeDelta << ")\n";
        return false;
    }

    return true;
}

bool expectMixedModeLeavesMinimumPhaseInputAlone() {
    wolfie::MeasurementSettings measurement;
    measurement.sampleRate = 48000;
    measurement.startFrequencyHz = 20.0;
    measurement.endFrequencyHz = 20000.0;

    wolfie::TargetCurveSettings targetCurve;
    wolfie::measurement::normalizeTargetCurveSettings(targetCurve, 20.0, 20000.0);

    wolfie::FilterDesignSettings minimumSettings;
    minimumSettings.tapCount = 16384;

    wolfie::FilterDesignSettings mixedSettings = minimumSettings;
    mixedSettings.phaseMode = "mixed";

    const wolfie::SmoothedResponse response = buildFlatResponse(0.0);
    const wolfie::MeasurementResult phaseMeasurement =
        buildPhaseMeasurement(measurement.sampleRate, 0.0);
    const wolfie::FilterDesignResult minimumResult =
        wolfie::measurement::designFilters(response,
                                           measurement,
                                           targetCurve,
                                           minimumSettings,
                                           &phaseMeasurement);
    const wolfie::FilterDesignResult mixedResult =
        wolfie::measurement::designFilters(response,
                                           measurement,
                                           targetCurve,
                                           mixedSettings,
                                           &phaseMeasurement);
    if (!minimumResult.valid || !mixedResult.valid || mixedResult.phaseMode != "mixed") {
        std::cerr << "mixed minimum-phase baseline did not produce valid results\n";
        return false;
    }

    const double leftPredictedBandMean = bandMeanAbs(mixedResult.frequencyAxisHz,
                                                     mixedResult.left.predictedExcessPhaseDegrees,
                                                     20.0,
                                                     300.0);
    const double rightPredictedBandMean = bandMeanAbs(mixedResult.frequencyAxisHz,
                                                      mixedResult.right.predictedExcessPhaseDegrees,
                                                      20.0,
                                                      300.0);
    if (leftPredictedBandMean > 2.0 || rightPredictedBandMean > 2.0) {
        std::cerr << "mixed mode introduced excess phase on a minimum-phase baseline (left="
                  << leftPredictedBandMean << ", right=" << rightPredictedBandMean << ")\n";
        return false;
    }

    const double leftMagnitudeDelta = bandMeanAbsDelta(mixedResult.frequencyAxisHz,
                                                       minimumResult.left.correctedResponseDb,
                                                       mixedResult.left.correctedResponseDb,
                                                       20.0,
                                                       20000.0);
    const double rightMagnitudeDelta = bandMeanAbsDelta(mixedResult.frequencyAxisHz,
                                                        minimumResult.right.correctedResponseDb,
                                                        mixedResult.right.correctedResponseDb,
                                                        20.0,
                                                        20000.0);
    if (leftMagnitudeDelta > 0.1 || rightMagnitudeDelta > 0.1) {
        std::cerr << "mixed mode changed magnitude on a minimum-phase baseline (left="
                  << leftMagnitudeDelta << ", right=" << rightMagnitudeDelta << ")\n";
        return false;
    }

    return true;
}

bool expectMixedModeIgnoresBulkDelay() {
    wolfie::MeasurementSettings measurement;
    measurement.sampleRate = 48000;
    measurement.startFrequencyHz = 20.0;
    measurement.endFrequencyHz = 20000.0;

    wolfie::TargetCurveSettings targetCurve;
    wolfie::measurement::normalizeTargetCurveSettings(targetCurve, 20.0, 20000.0);

    wolfie::FilterDesignSettings filterSettings;
    filterSettings.tapCount = 16384;
    filterSettings.phaseMode = "mixed";

    const wolfie::SmoothedResponse response = buildFlatResponse(0.0);
    const wolfie::MeasurementResult phaseMeasurement =
        buildPhaseMeasurement(measurement.sampleRate, 0.0065);
    const wolfie::FilterDesignResult result =
        wolfie::measurement::designFilters(response,
                                           measurement,
                                           targetCurve,
                                           filterSettings,
                                           &phaseMeasurement);
    if (!result.valid) {
        std::cerr << "mixed bulk-delay baseline did not produce a valid filter result\n";
        return false;
    }

    const double leftPredictedBandMean = bandMeanAbs(result.frequencyAxisHz,
                                                     result.left.predictedExcessPhaseDegrees,
                                                     20.0,
                                                     300.0);
    const double rightPredictedBandMean = bandMeanAbs(result.frequencyAxisHz,
                                                      result.right.predictedExcessPhaseDegrees,
                                                      20.0,
                                                      300.0);
    if (leftPredictedBandMean > 3.0 || rightPredictedBandMean > 3.0) {
        std::cerr << "mixed mode reacted to pure bulk delay (left="
                  << leftPredictedBandMean << ", right=" << rightPredictedBandMean << ")\n";
        return false;
    }

    return true;
}

bool expectMixedModeReducesLowFrequencyExcessPhase() {
    wolfie::MeasurementSettings measurement;
    measurement.sampleRate = 48000;
    measurement.startFrequencyHz = 20.0;
    measurement.endFrequencyHz = 20000.0;

    wolfie::TargetCurveSettings targetCurve;
    wolfie::measurement::normalizeTargetCurveSettings(targetCurve, 20.0, 20000.0);

    wolfie::FilterDesignSettings filterSettings;
    filterSettings.tapCount = 16384;
    filterSettings.phaseMode = "mixed";

    const wolfie::SmoothedResponse response = buildFlatResponse(0.0);
    const wolfie::MeasurementResult phaseMeasurement =
        buildPhaseMeasurement(measurement.sampleRate, 0.0, 1.0, 0.0);
    const wolfie::FilterDesignResult result =
        wolfie::measurement::designFilters(response,
                                           measurement,
                                           targetCurve,
                                           filterSettings,
                                           &phaseMeasurement);
    if (!result.valid) {
        std::cerr << "mixed reduction case did not produce a valid filter result\n";
        return false;
    }

    const double leftInputBandMean = bandMeanAbs(result.frequencyAxisHz,
                                                 result.left.inputExcessPhaseDegrees,
                                                 20.0,
                                                 200.0);
    const double leftPredictedBandMean = bandMeanAbs(result.frequencyAxisHz,
                                                     result.left.predictedExcessPhaseDegrees,
                                                     20.0,
                                                     200.0);
    if (leftInputBandMean < 8.0) {
        std::cerr << "synthetic mixed-mode fixture did not produce a meaningful LF phase error\n";
        return false;
    }
    if (leftPredictedBandMean > leftInputBandMean * 0.6) {
        std::cerr << "mixed mode did not materially reduce LF excess phase (before="
                  << leftInputBandMean << ", after=" << leftPredictedBandMean << ")\n";
        return false;
    }

    const double rightPredictedBandMean = bandMeanAbs(result.frequencyAxisHz,
                                                      result.right.predictedExcessPhaseDegrees,
                                                      20.0,
                                                      200.0);
    if (rightPredictedBandMean > 2.0) {
        std::cerr << "mixed mode changed the clean channel while correcting the left channel (right="
                  << rightPredictedBandMean << ")\n";
        return false;
    }

    return true;
}

bool expectMixedModeContainsCorrectionToLowFrequencies() {
    wolfie::MeasurementSettings measurement;
    measurement.sampleRate = 48000;
    measurement.startFrequencyHz = 20.0;
    measurement.endFrequencyHz = 20000.0;

    wolfie::TargetCurveSettings targetCurve;
    wolfie::measurement::normalizeTargetCurveSettings(targetCurve, 20.0, 20000.0);

    wolfie::FilterDesignSettings filterSettings;
    filterSettings.tapCount = 16384;
    filterSettings.phaseMode = "mixed";

    const wolfie::SmoothedResponse response = buildFlatResponse(0.0);
    const wolfie::MeasurementResult phaseMeasurement =
        buildPhaseMeasurement(measurement.sampleRate, 0.0, 1.0, 0.0);
    const wolfie::FilterDesignResult result =
        wolfie::measurement::designFilters(response,
                                           measurement,
                                           targetCurve,
                                           filterSettings,
                                           &phaseMeasurement);
    if (!result.valid) {
        std::cerr << "mixed containment case did not produce a valid filter result\n";
        return false;
    }

    const double highBandDelta = bandMeanAbsDelta(result.frequencyAxisHz,
                                                  result.left.inputExcessPhaseDegrees,
                                                  result.left.predictedExcessPhaseDegrees,
                                                  500.0,
                                                  5000.0);
    if (highBandDelta > 5.0) {
        std::cerr << "mixed mode changed too much phase out of band (" << highBandDelta << " deg)\n";
        return false;
    }

    return true;
}

bool expectMixedModePreservesMagnitudeVsMinimum() {
    wolfie::MeasurementSettings measurement;
    measurement.sampleRate = 48000;
    measurement.startFrequencyHz = 20.0;
    measurement.endFrequencyHz = 20000.0;

    wolfie::TargetCurveSettings targetCurve;
    targetCurve.lowGainDb = 2.0;
    targetCurve.midGainDb = 0.0;
    targetCurve.highGainDb = -1.5;
    wolfie::measurement::normalizeTargetCurveSettings(targetCurve, 20.0, 20000.0);

    wolfie::FilterDesignSettings minimumSettings;
    minimumSettings.tapCount = 16384;
    minimumSettings.maxBoostDb = 6.0;
    minimumSettings.maxCutDb = 12.0;
    minimumSettings.highCorrectionHz = 18000.0;

    wolfie::FilterDesignSettings mixedSettings = minimumSettings;
    mixedSettings.phaseMode = "mixed";

    const wolfie::SmoothedResponse response = buildSyntheticResponse();
    const wolfie::MeasurementResult phaseMeasurement =
        buildPhaseMeasurement(measurement.sampleRate, 0.0, 1.0, 0.0);
    const wolfie::FilterDesignResult minimumResult =
        wolfie::measurement::designFilters(response,
                                           measurement,
                                           targetCurve,
                                           minimumSettings,
                                           &phaseMeasurement);
    const wolfie::FilterDesignResult mixedResult =
        wolfie::measurement::designFilters(response,
                                           measurement,
                                           targetCurve,
                                           mixedSettings,
                                           &phaseMeasurement);
    if (!minimumResult.valid || !mixedResult.valid) {
        std::cerr << "mixed magnitude-preservation case did not produce valid filter results\n";
        return false;
    }

    const double leftMagnitudeDelta = bandMeanAbsDelta(mixedResult.frequencyAxisHz,
                                                       minimumResult.left.correctedResponseDb,
                                                       mixedResult.left.correctedResponseDb,
                                                       20.0,
                                                       20000.0);
    const double rightMagnitudeDelta = bandMeanAbsDelta(mixedResult.frequencyAxisHz,
                                                        minimumResult.right.correctedResponseDb,
                                                        mixedResult.right.correctedResponseDb,
                                                        20.0,
                                                        20000.0);
    if (leftMagnitudeDelta > 0.25 || rightMagnitudeDelta > 0.25) {
        std::cerr << "mixed mode changed magnitude too much versus minimum phase (left="
                  << leftMagnitudeDelta << ", right=" << rightMagnitudeDelta << ")\n";
        return false;
    }

    return true;
}

bool expectMixedModeStrengthZeroMatchesMinimum() {
    wolfie::MeasurementSettings measurement;
    measurement.sampleRate = 48000;
    measurement.startFrequencyHz = 20.0;
    measurement.endFrequencyHz = 20000.0;

    wolfie::TargetCurveSettings targetCurve;
    wolfie::measurement::normalizeTargetCurveSettings(targetCurve, 20.0, 20000.0);

    wolfie::FilterDesignSettings minimumSettings;
    minimumSettings.tapCount = 16384;

    wolfie::FilterDesignSettings mixedSettings = minimumSettings;
    mixedSettings.phaseMode = "mixed";
    mixedSettings.mixedPhaseStrength = 0.0;

    const wolfie::SmoothedResponse response = buildFlatResponse(0.0);
    const wolfie::MeasurementResult phaseMeasurement =
        buildPhaseMeasurement(measurement.sampleRate, 0.0, 1.0, 0.0);
    const wolfie::FilterDesignResult minimumResult =
        wolfie::measurement::designFilters(response,
                                           measurement,
                                           targetCurve,
                                           minimumSettings,
                                           &phaseMeasurement);
    const wolfie::FilterDesignResult mixedResult =
        wolfie::measurement::designFilters(response,
                                           measurement,
                                           targetCurve,
                                           mixedSettings,
                                           &phaseMeasurement);
    if (!minimumResult.valid || !mixedResult.valid) {
        std::cerr << "mixed strength-zero case did not produce valid filter results\n";
        return false;
    }

    const double leftMagnitudeDelta = bandMeanAbsDelta(mixedResult.frequencyAxisHz,
                                                       minimumResult.left.correctedResponseDb,
                                                       mixedResult.left.correctedResponseDb,
                                                       20.0,
                                                       20000.0);
    const double rightMagnitudeDelta = bandMeanAbsDelta(mixedResult.frequencyAxisHz,
                                                        minimumResult.right.correctedResponseDb,
                                                        mixedResult.right.correctedResponseDb,
                                                        20.0,
                                                        20000.0);
    if (leftMagnitudeDelta > 0.1 || rightMagnitudeDelta > 0.1) {
        std::cerr << "mixed strength zero changed magnitude versus minimum phase (left="
                  << leftMagnitudeDelta << ", right=" << rightMagnitudeDelta << ")\n";
        return false;
    }

    const double leftResidualDelta = bandMeanAbsDelta(mixedResult.frequencyAxisHz,
                                                      minimumResult.left.predictedExcessPhaseDegrees,
                                                      mixedResult.left.predictedExcessPhaseDegrees,
                                                      20.0,
                                                      300.0);
    const double rightResidualDelta = bandMeanAbsDelta(mixedResult.frequencyAxisHz,
                                                       minimumResult.right.predictedExcessPhaseDegrees,
                                                       mixedResult.right.predictedExcessPhaseDegrees,
                                                       20.0,
                                                       300.0);
    if (leftResidualDelta > 2.0 || rightResidualDelta > 2.0) {
        std::cerr << "mixed strength zero changed predicted excess phase versus minimum phase (left="
                  << leftResidualDelta << ", right=" << rightResidualDelta << ")\n";
        return false;
    }

    return true;
}

bool expectMixedModePhaseLimitControlsCorrectionExtent() {
    wolfie::MeasurementSettings measurement;
    measurement.sampleRate = 48000;
    measurement.startFrequencyHz = 20.0;
    measurement.endFrequencyHz = 20000.0;

    wolfie::TargetCurveSettings targetCurve;
    wolfie::measurement::normalizeTargetCurveSettings(targetCurve, 20.0, 20000.0);

    wolfie::FilterDesignSettings narrowSettings;
    narrowSettings.tapCount = 16384;
    narrowSettings.phaseMode = "mixed";
    narrowSettings.mixedPhaseMaxFrequencyHz = 120.0;

    wolfie::FilterDesignSettings wideSettings = narrowSettings;
    wideSettings.mixedPhaseMaxFrequencyHz = 320.0;

    const wolfie::SmoothedResponse response = buildFlatResponse(0.0);
    const wolfie::MeasurementResult phaseMeasurement =
        buildPhaseMeasurement(measurement.sampleRate, 0.0, 1.0, 0.0);
    const wolfie::FilterDesignResult narrowResult =
        wolfie::measurement::designFilters(response,
                                           measurement,
                                           targetCurve,
                                           narrowSettings,
                                           &phaseMeasurement);
    const wolfie::FilterDesignResult wideResult =
        wolfie::measurement::designFilters(response,
                                           measurement,
                                           targetCurve,
                                           wideSettings,
                                           &phaseMeasurement);
    if (!narrowResult.valid || !wideResult.valid) {
        std::cerr << "mixed phase-limit case did not produce valid filter results\n";
        return false;
    }

    const double inputLowBand = bandMeanAbs(narrowResult.frequencyAxisHz,
                                            narrowResult.left.inputExcessPhaseDegrees,
                                            20.0,
                                            90.0);
    const double narrowLowBand = bandMeanAbs(narrowResult.frequencyAxisHz,
                                             narrowResult.left.predictedExcessPhaseDegrees,
                                             20.0,
                                             90.0);
    if (narrowLowBand > inputLowBand * 0.75) {
        std::cerr << "narrow mixed phase limit stopped useful low-band reduction (before="
                  << inputLowBand << ", after=" << narrowLowBand << ")\n";
        return false;
    }

    const double narrowUpperBand = bandMeanAbs(narrowResult.frequencyAxisHz,
                                               narrowResult.left.predictedExcessPhaseDegrees,
                                               110.0,
                                               220.0);
    const double wideUpperBand = bandMeanAbs(wideResult.frequencyAxisHz,
                                             wideResult.left.predictedExcessPhaseDegrees,
                                             110.0,
                                             220.0);
    if (wideUpperBand > narrowUpperBand * 0.5) {
        std::cerr << "mixed phase limit did not change the upper LF correction extent (narrow="
                  << narrowUpperBand << ", wide=" << wideUpperBand << ")\n";
        return false;
    }

    return true;
}

bool expectMixedModePhaseCapControlsLowFrequencyReduction() {
    wolfie::MeasurementSettings measurement;
    measurement.sampleRate = 48000;
    measurement.startFrequencyHz = 20.0;
    measurement.endFrequencyHz = 20000.0;

    wolfie::TargetCurveSettings targetCurve;
    wolfie::measurement::normalizeTargetCurveSettings(targetCurve, 20.0, 20000.0);

    wolfie::FilterDesignSettings lowCapSettings;
    lowCapSettings.tapCount = 16384;
    lowCapSettings.phaseMode = "mixed";
    lowCapSettings.mixedPhaseMaxFrequencyHz = 220.0;
    lowCapSettings.mixedPhaseMaxCorrectionDegrees = 120.0;

    wolfie::FilterDesignSettings highCapSettings = lowCapSettings;
    highCapSettings.mixedPhaseMaxCorrectionDegrees = 360.0;

    const wolfie::SmoothedResponse response = buildFlatResponse(0.0);
    const wolfie::MeasurementResult phaseMeasurement =
        buildPhaseMeasurement(measurement.sampleRate, 0.0, 3.0, 0.0);
    const wolfie::FilterDesignResult lowCapResult =
        wolfie::measurement::designFilters(response,
                                           measurement,
                                           targetCurve,
                                           lowCapSettings,
                                           &phaseMeasurement);
    const wolfie::FilterDesignResult highCapResult =
        wolfie::measurement::designFilters(response,
                                           measurement,
                                           targetCurve,
                                           highCapSettings,
                                           &phaseMeasurement);
    if (!lowCapResult.valid || !highCapResult.valid) {
        std::cerr << "mixed phase-cap case did not produce valid filter results\n";
        return false;
    }

    const double inputLowBand = bandMeanAbs(lowCapResult.frequencyAxisHz,
                                            lowCapResult.left.inputExcessPhaseDegrees,
                                            20.0,
                                            80.0);
    const double lowCapResidual = bandMeanAbs(lowCapResult.frequencyAxisHz,
                                              lowCapResult.left.predictedExcessPhaseDegrees,
                                              20.0,
                                              80.0);
    const double highCapResidual = bandMeanAbs(highCapResult.frequencyAxisHz,
                                               highCapResult.left.predictedExcessPhaseDegrees,
                                               20.0,
                                               80.0);
    if (inputLowBand < 40.0) {
        std::cerr << "synthetic mixed phase-cap fixture did not produce enough LF excess phase\n";
        return false;
    }
    if (highCapResidual > lowCapResidual * 0.9) {
        std::cerr << "raising mixed phase cap did not materially improve LF reduction (low="
                  << lowCapResidual << ", high=" << highCapResidual << ")\n";
        return false;
    }

    return true;
}

bool expectMixedModeStereoImpulsePeaksShareOneLatency() {
    wolfie::MeasurementSettings measurement;
    measurement.sampleRate = 48000;
    measurement.startFrequencyHz = 20.0;
    measurement.endFrequencyHz = 20000.0;

    wolfie::TargetCurveSettings targetCurve;
    wolfie::measurement::normalizeTargetCurveSettings(targetCurve, 20.0, 20000.0);

    wolfie::FilterDesignSettings filterSettings;
    filterSettings.tapCount = 16384;
    filterSettings.phaseMode = "mixed";
    filterSettings.mixedPhaseMaxCorrectionDegrees = 720.0;

    const wolfie::SmoothedResponse response = buildFlatResponse(0.0);
    const wolfie::MeasurementResult phaseMeasurement =
        buildPhaseMeasurement(measurement.sampleRate, 0.0, 3.0, -2.0);
    const wolfie::FilterDesignResult result =
        wolfie::measurement::designFilters(response,
                                           measurement,
                                           targetCurve,
                                           filterSettings,
                                           &phaseMeasurement);
    if (!result.valid) {
        std::cerr << "mixed stereo-latency alignment case did not produce a valid filter result\n";
        return false;
    }

    const int expectedPeakIndex = filterSettings.tapCount / 2;
    const int stereoPeakDelta = std::abs(result.left.impulsePeakIndex - result.right.impulsePeakIndex);
    if (stereoPeakDelta > 1) {
        std::cerr << "mixed stereo filter peaks diverged in latency (left="
                  << result.left.impulsePeakIndex << ", right=" << result.right.impulsePeakIndex << ")\n";
        return false;
    }
    if (std::abs(result.left.impulsePeakIndex - expectedPeakIndex) > 2 ||
        std::abs(result.right.impulsePeakIndex - expectedPeakIndex) > 2) {
        std::cerr << "mixed stereo filter peaks were not centered to a shared latency (left="
                  << result.left.impulsePeakIndex << ", right=" << result.right.impulsePeakIndex << ")\n";
        return false;
    }

    return true;
}

bool expectInputGroupDelayIsPublishedFromMeasuredPhase() {
    wolfie::MeasurementSettings measurement;
    measurement.sampleRate = 48000;
    measurement.startFrequencyHz = 20.0;
    measurement.endFrequencyHz = 20000.0;

    wolfie::TargetCurveSettings targetCurve;
    wolfie::measurement::normalizeTargetCurveSettings(targetCurve, 20.0, 20000.0);

    wolfie::FilterDesignSettings filterSettings;
    filterSettings.tapCount = 16384;

    const wolfie::SmoothedResponse response = buildFlatResponse(0.0);
    filterSettings.phaseMode = "mixed";

    const wolfie::MeasurementResult phaseMeasurement =
        buildPhaseMeasurement(measurement.sampleRate, 0.0, 1.0, 0.0);
    const wolfie::FilterDesignResult result =
        wolfie::measurement::designFilters(response,
                                           measurement,
                                           targetCurve,
                                           filterSettings,
                                           &phaseMeasurement);
    if (!result.valid) {
        std::cerr << "input group-delay publication case did not produce a valid filter result\n";
        return false;
    }

    if (result.left.inputGroupDelayMs.size() != result.frequencyAxisHz.size() ||
        result.right.inputGroupDelayMs.size() != result.frequencyAxisHz.size()) {
        std::cerr << "measured input group delay was not published at display resolution\n";
        return false;
    }

    const double leftInputDelay = bandMeanAbs(result.frequencyAxisHz,
                                              result.left.inputGroupDelayMs,
                                              20.0,
                                              200.0);
    const double rightInputDelay = bandMeanAbs(result.frequencyAxisHz,
                                               result.right.inputGroupDelayMs,
                                               20.0,
                                               200.0);
    if (leftInputDelay < 0.02 || rightInputDelay > 0.01) {
        std::cerr << "measured input group delay was not preserved in the filter result (left="
                  << leftInputDelay << ", right=" << rightInputDelay << ")\n";
        return false;
    }

    return true;
}

bool expectContinuousExcessPhaseSeriesStaySmoothAcrossWraps() {
    wolfie::MeasurementSettings measurement;
    measurement.sampleRate = 48000;
    measurement.startFrequencyHz = 20.0;
    measurement.endFrequencyHz = 20000.0;

    wolfie::TargetCurveSettings targetCurve;
    wolfie::measurement::normalizeTargetCurveSettings(targetCurve, 20.0, 20000.0);

    wolfie::FilterDesignSettings filterSettings;
    filterSettings.tapCount = 16384;
    filterSettings.phaseMode = "mixed";
    filterSettings.mixedPhaseMaxCorrectionDegrees = 360.0;

    const wolfie::SmoothedResponse response = buildFlatResponse(0.0);
    const wolfie::MeasurementResult phaseMeasurement =
        buildPhaseMeasurement(measurement.sampleRate, 0.0, 3.0, 0.0);
    const wolfie::FilterDesignResult result =
        wolfie::measurement::designFilters(response,
                                           measurement,
                                           targetCurve,
                                           filterSettings,
                                           &phaseMeasurement);
    if (!result.valid) {
        std::cerr << "continuous excess-phase publication case did not produce a valid filter result\n";
        return false;
    }

    const double inputContinuousJump = maxAdjacentAbsDelta(result.left.inputExcessPhaseContinuousDegrees);
    const double predictedContinuousJump = maxAdjacentAbsDelta(result.left.predictedExcessPhaseContinuousDegrees);
    if (inputContinuousJump > 120.0 || predictedContinuousJump > 120.0) {
        std::cerr << "continuous excess-phase series contains implausible wrap jumps (input="
                  << inputContinuousJump << ", predicted=" << predictedContinuousJump << ")\n";
        return false;
    }

    return true;
}

bool expectRoonExportSupportsCommonSampleRates() {
    wolfie::MeasurementSettings measurement;
    measurement.sampleRate = 48000;
    measurement.startFrequencyHz = 20.0;
    measurement.endFrequencyHz = 20000.0;

    wolfie::TargetCurveSettings targetCurve;
    wolfie::measurement::normalizeTargetCurveSettings(targetCurve, 20.0, 20000.0);

    wolfie::FilterDesignSettings filterSettings;
    filterSettings.tapCount = 16384;
    filterSettings.maxBoostDb = 6.0;
    filterSettings.maxCutDb = 12.0;

    const wolfie::SmoothedResponse response = buildSyntheticResponse();
    const std::filesystem::path exportDirectory =
        std::filesystem::temp_directory_path() / "wolfie-roon-export-test";
    std::error_code cleanupError;
    std::filesystem::remove_all(exportDirectory, cleanupError);

    std::vector<std::filesystem::path> generatedFiles;
    std::wstring errorMessage;
    const bool exported = wolfie::measurement::exportRoonFilterWavSet(exportDirectory,
                                                                      response,
                                                                      measurement,
                                                                      targetCurve,
                                                                      filterSettings,
                                                                      nullptr,
                                                                      wolfie::measurement::roonCommonSampleRates(),
                                                                      generatedFiles,
                                                                      errorMessage);
    if (!exported) {
        std::wcerr << L"Roon export failed: " << errorMessage << L"\n";
        return false;
    }

    if (generatedFiles.size() != (wolfie::measurement::roonCommonSampleRates().size() * 2) + 1) {
        std::cerr << "Roon export did not generate the expected number of files\n";
        return false;
    }

    for (const int sampleRate : wolfie::measurement::roonCommonSampleRates()) {
        const std::filesystem::path wavPath =
            wolfie::measurement::roonFilterWavPath(exportDirectory, sampleRate);
        if (!std::filesystem::exists(wavPath) || std::filesystem::file_size(wavPath) <= 44) {
            std::cerr << "Roon export did not write a valid WAV for " << sampleRate << " Hz\n";
            return false;
        }

        const std::filesystem::path cfgPath =
            wolfie::measurement::roonFilterConfigPath(exportDirectory, sampleRate);
        if (!std::filesystem::exists(cfgPath) || std::filesystem::file_size(cfgPath) == 0) {
            std::cerr << "Roon export did not write a config for " << sampleRate << " Hz\n";
            return false;
        }

        std::ifstream cfg(cfgPath, std::ios::binary);
        std::ostringstream cfgText;
        cfgText << cfg.rdbuf();
        const std::string expected =
            std::to_string(sampleRate) + " 2 2 0\n"
            "0 0\n"
            "0 0\n" +
            wavPath.filename().string() + "\n"
            "0\n"
            "0.0\n"
            "0.0\n" +
            wavPath.filename().string() + "\n"
            "1\n"
            "1.0\n"
            "1.0\n";
        if (cfgText.str() != expected) {
            std::cerr << "Roon config contents were unexpected for " << sampleRate << " Hz\n";
            return false;
        }
    }

    const std::filesystem::path zipPath = wolfie::measurement::roonFilterArchivePath(exportDirectory);
    if (!std::filesystem::exists(zipPath) || std::filesystem::file_size(zipPath) == 0) {
        std::cerr << "Roon export did not write roon.zip\n";
        return false;
    }

    {
        std::ifstream zip(zipPath, std::ios::binary);
        std::ostringstream zipText;
        zipText << zip.rdbuf();
        const std::string zipContents = zipText.str();
        for (const int sampleRate : wolfie::measurement::roonCommonSampleRates()) {
            const std::string wavName = wolfie::measurement::roonFilterWavPath(exportDirectory, sampleRate).filename().string();
            const std::string cfgName = wolfie::measurement::roonFilterConfigPath(exportDirectory, sampleRate).filename().string();
            if (zipContents.find(wavName) == std::string::npos || zipContents.find(cfgName) == std::string::npos) {
                std::cerr << "Roon archive is missing an exported file name for " << sampleRate << " Hz\n";
                return false;
            }
        }
    }

    cleanupError.clear();
    std::filesystem::remove_all(exportDirectory, cleanupError);
    return true;
}

bool expectRoonMixedExportDiffersFromMinimum() {
    wolfie::MeasurementSettings measurement;
    measurement.sampleRate = 48000;
    measurement.startFrequencyHz = 20.0;
    measurement.endFrequencyHz = 20000.0;

    wolfie::TargetCurveSettings targetCurve;
    wolfie::measurement::normalizeTargetCurveSettings(targetCurve, 20.0, 20000.0);

    wolfie::FilterDesignSettings minimumSettings;
    minimumSettings.tapCount = 16384;
    minimumSettings.maxBoostDb = 6.0;
    minimumSettings.maxCutDb = 12.0;

    wolfie::FilterDesignSettings mixedSettings = minimumSettings;
    mixedSettings.phaseMode = "mixed";

    const wolfie::SmoothedResponse response = buildSyntheticResponse();
    const wolfie::MeasurementResult phaseMeasurement =
        buildPhaseMeasurement(measurement.sampleRate, 0.0, 3.0, 2.0);
    const std::vector<int> sampleRates = {48000};
    const std::filesystem::path rootDirectory =
        std::filesystem::temp_directory_path() / "wolfie-roon-export-mixed-phase-test";
    const std::filesystem::path minimumDirectory = rootDirectory / "minimum";
    const std::filesystem::path mixedDirectory = rootDirectory / "mixed";
    std::error_code cleanupError;
    std::filesystem::remove_all(rootDirectory, cleanupError);

    std::vector<std::filesystem::path> generatedFiles;
    std::wstring errorMessage;
    const bool minimumExported = wolfie::measurement::exportRoonFilterWavSet(minimumDirectory,
                                                                             response,
                                                                             measurement,
                                                                             targetCurve,
                                                                             minimumSettings,
                                                                             &phaseMeasurement,
                                                                             sampleRates,
                                                                             generatedFiles,
                                                                             errorMessage);
    if (!minimumExported) {
        std::wcerr << L"Minimum-phase Roon export failed: " << errorMessage << L"\n";
        return false;
    }

    generatedFiles.clear();
    errorMessage.clear();
    const bool mixedExported = wolfie::measurement::exportRoonFilterWavSet(mixedDirectory,
                                                                           response,
                                                                           measurement,
                                                                           targetCurve,
                                                                           mixedSettings,
                                                                           &phaseMeasurement,
                                                                           sampleRates,
                                                                           generatedFiles,
                                                                           errorMessage);
    if (!mixedExported) {
        std::wcerr << L"Mixed-phase Roon export failed: " << errorMessage << L"\n";
        return false;
    }

    const std::vector<char> minimumBytes =
        readFileBytes(wolfie::measurement::roonFilterWavPath(minimumDirectory, sampleRates.front()));
    const std::vector<char> mixedBytes =
        readFileBytes(wolfie::measurement::roonFilterWavPath(mixedDirectory, sampleRates.front()));
    if (minimumBytes.empty() || mixedBytes.empty()) {
        std::cerr << "Mixed-phase export regression test could not read exported WAV files\n";
        return false;
    }
    if (minimumBytes == mixedBytes) {
        std::cerr << "Mixed-phase export produced the same WAV as minimum phase\n";
        return false;
    }

    cleanupError.clear();
    std::filesystem::remove_all(rootDirectory, cleanupError);
    return true;
}

}  // namespace

bool runFilterDesignTests() {
    return expectDesignedFilterLooksSane() &&
           expectExactTargetCurveEvaluationCapturesBellPeak() &&
           expectTargetCurveAnchorsToMeasuredLevel() &&
           expectMinimumPhaseInputNeedsNoExcessCorrection() &&
           expectBulkDelayIsNotTreatedAsExcessPhase() &&
           expectExcessLfModeLeavesMinimumPhaseInputAlone() &&
           expectExcessLfModeIgnoresBulkDelay() &&
           expectExcessLfModeReducesLowFrequencyExcessPhase() &&
           expectExcessLfModeContainsCorrectionToLowFrequencies() &&
           expectMixedModeLeavesMinimumPhaseInputAlone() &&
           expectMixedModeIgnoresBulkDelay() &&
           expectMixedModeReducesLowFrequencyExcessPhase() &&
           expectMixedModeContainsCorrectionToLowFrequencies() &&
           expectMixedModePreservesMagnitudeVsMinimum() &&
           expectMixedModeStrengthZeroMatchesMinimum() &&
           expectMixedModePhaseLimitControlsCorrectionExtent() &&
           expectMixedModePhaseCapControlsLowFrequencyReduction() &&
           expectMixedModeStereoImpulsePeaksShareOneLatency() &&
           expectInputGroupDelayIsPublishedFromMeasuredPhase() &&
           expectContinuousExcessPhaseSeriesStaySmoothAcrossWraps() &&
           expectRoonExportSupportsCommonSampleRates() &&
           expectRoonMixedExportDiffersFromMinimum();
}
