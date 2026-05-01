#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "core/models.h"

namespace wolfie::tests {

std::vector<double> buildLogAxis(double minFrequencyHz, double maxFrequencyHz, int pointCount);
wolfie::SmoothedResponse buildSyntheticResponse();
wolfie::SmoothedResponse buildFlatResponse(double levelDb);
wolfie::SmoothedResponse buildLowFrequencyRollOffResponse();

double wrapDegrees(double phaseDegrees);
wolfie::MeasurementValueSet buildImpulseValueSet(double leadingTimeSeconds);
wolfie::MeasurementValueSet buildWrappedPhaseSpectrum(const std::vector<double>& frequencyAxisHz,
                                                      const std::string& key,
                                                      double delaySeconds,
                                                      double leftExcessScale,
                                                      double rightExcessScale);
wolfie::MeasurementValueSet buildFlatMagnitudeSpectrum(const std::vector<double>& frequencyAxisHz,
                                                       const std::string& key,
                                                       double leftLevelDb = 0.0,
                                                       double rightLevelDb = 0.0);
std::vector<double> buildLinearAxis(double maxFrequencyHz, int pointCount);
wolfie::MeasurementResult buildPhaseMeasurement(int sampleRate,
                                                double delaySeconds,
                                                double leftExcessScale = 0.0,
                                                double rightExcessScale = 0.0);
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
                                                                      double rawRightExcessScale);

std::vector<char> readFileBytes(const std::filesystem::path& path);
double bandMeanAbs(const std::vector<double>& frequencyAxisHz,
                   const std::vector<double>& values,
                   double minFrequencyHz,
                   double maxFrequencyHz);
double bandMeanAbsDelta(const std::vector<double>& frequencyAxisHz,
                        const std::vector<double>& leftValues,
                        const std::vector<double>& rightValues,
                        double minFrequencyHz,
                        double maxFrequencyHz);
double maxAdjacentAbsDelta(const std::vector<double>& values);
bool processLogContains(const wolfie::FilterDesignResult& result, const std::string& needle);

}  // namespace wolfie::tests
