#include "measurement/phase_preparation.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>
#include <string_view>

#include "measurement/dsp_utils.h"
#include "measurement/response_smoother.h"

namespace wolfie::measurement {

namespace {

constexpr double kRadiansToDegrees = 180.0 / std::numbers::pi_v<double>;
constexpr double kDegreesToRadians = std::numbers::pi_v<double> / 180.0;

template <typename T>
T clampValue(T value, T low, T high) {
    return std::max(low, std::min(value, high));
}

double interpolateLinear(double x, double x0, double y0, double x1, double y1) {
    if (std::abs(x1 - x0) < 1.0e-12) {
        return y1;
    }
    const double t = clampValue((x - x0) / (x1 - x0), 0.0, 1.0);
    return y0 + ((y1 - y0) * t);
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

    const auto it = std::lower_bound(frequencyAxisHz.begin(),
                                     frequencyAxisHz.begin() + static_cast<std::ptrdiff_t>(count),
                                     frequencyHz);
    const size_t upperIndex = static_cast<size_t>(std::distance(frequencyAxisHz.begin(), it));
    if (upperIndex == 0) {
        return values.front();
    }

    const size_t lowerIndex = upperIndex - 1;
    const double x0 = std::log10(std::max(frequencyAxisHz[lowerIndex], 1.0));
    const double x1 = std::log10(std::max(frequencyAxisHz[upperIndex], 1.0));
    const double x = std::log10(std::max(frequencyHz, 1.0));
    return interpolateLinear(x, x0, values[lowerIndex], x1, values[upperIndex]);
}

double interpolateLinearFrequency(const std::vector<double>& frequencyAxisHz,
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

    const auto it = std::lower_bound(frequencyAxisHz.begin(),
                                     frequencyAxisHz.begin() + static_cast<std::ptrdiff_t>(count),
                                     frequencyHz);
    const size_t upperIndex = static_cast<size_t>(std::distance(frequencyAxisHz.begin(), it));
    if (upperIndex == 0) {
        return values.front();
    }

    const size_t lowerIndex = upperIndex - 1;
    return interpolateLinear(frequencyHz,
                             frequencyAxisHz[lowerIndex],
                             values[lowerIndex],
                             frequencyAxisHz[upperIndex],
                             values[upperIndex]);
}

double interpolateFrequency(const std::vector<double>& frequencyAxisHz,
                            const std::vector<double>& values,
                            double frequencyHz,
                            bool sourceOnLogAxis) {
    return sourceOnLogAxis ? interpolateLogFrequency(frequencyAxisHz, values, frequencyHz)
                           : interpolateLinearFrequency(frequencyAxisHz, values, frequencyHz);
}

std::vector<double> resampleLogFrequency(const std::vector<double>& sourceAxisHz,
                                         const std::vector<double>& sourceValues,
                                         const std::vector<double>& destinationAxisHz) {
    std::vector<double> values;
    values.reserve(destinationAxisHz.size());
    for (const double frequencyHz : destinationAxisHz) {
        values.push_back(interpolateLogFrequency(sourceAxisHz, sourceValues, frequencyHz));
    }
    return values;
}

std::vector<double> degreesToRadians(const std::vector<double>& degrees) {
    std::vector<double> radians;
    radians.reserve(degrees.size());
    for (const double value : degrees) {
        radians.push_back(value * kDegreesToRadians);
    }
    return radians;
}

std::vector<double> radiansToDegrees(const std::vector<double>& radians) {
    std::vector<double> degrees;
    degrees.reserve(radians.size());
    for (const double value : radians) {
        degrees.push_back(value * kRadiansToDegrees);
    }
    return degrees;
}

std::vector<double> principalPhaseDifferenceRadians(const std::vector<double>& leftRadians,
                                                    const std::vector<double>& rightRadians) {
    const size_t count = std::min(leftRadians.size(), rightRadians.size());
    std::vector<double> result;
    result.reserve(count);
    for (size_t index = 0; index < count; ++index) {
        double difference = std::remainder(leftRadians[index] - rightRadians[index],
                                           2.0 * std::numbers::pi_v<double>);
        if (difference <= -std::numbers::pi_v<double>) {
            difference += 2.0 * std::numbers::pi_v<double>;
        } else if (difference > std::numbers::pi_v<double>) {
            difference -= 2.0 * std::numbers::pi_v<double>;
        }
        result.push_back(difference);
    }
    return result;
}

std::vector<double> wrapPhaseRadiansSeriesDegrees(const std::vector<double>& phaseRadians) {
    std::vector<double> degrees;
    degrees.reserve(phaseRadians.size());
    for (const double phase : phaseRadians) {
        double wrapped = std::remainder(phase, 2.0 * std::numbers::pi_v<double>);
        if (wrapped <= -std::numbers::pi_v<double>) {
            wrapped += 2.0 * std::numbers::pi_v<double>;
        } else if (wrapped > std::numbers::pi_v<double>) {
            wrapped -= 2.0 * std::numbers::pi_v<double>;
        }
        degrees.push_back(wrapped * kRadiansToDegrees);
    }
    return degrees;
}

void removeLinearDelay(std::vector<double>& phaseRadians,
                       const std::vector<double>& frequencyAxisHz,
                       double delaySeconds) {
    if (!std::isfinite(delaySeconds) || std::abs(delaySeconds) <= 1.0e-12) {
        return;
    }

    const size_t count = std::min(phaseRadians.size(), frequencyAxisHz.size());
    for (size_t index = 0; index < count; ++index) {
        phaseRadians[index] += 2.0 * std::numbers::pi_v<double> * frequencyAxisHz[index] * delaySeconds;
    }
}

struct WindowedTransferData {
    std::vector<double> frequencyAxisHz;
    std::vector<double> magnitudeDb;
    std::vector<double> phaseRadians;

    [[nodiscard]] bool valid() const {
        return !frequencyAxisHz.empty() &&
               magnitudeDb.size() == frequencyAxisHz.size() &&
               phaseRadians.size() == frequencyAxisHz.size();
    }
};

size_t maxAbsIndex(const std::vector<double>& samples) {
    if (samples.empty()) {
        return 0;
    }

    size_t bestIndex = 0;
    double bestMagnitude = 0.0;
    for (size_t index = 0; index < samples.size(); ++index) {
        const double magnitude = std::abs(samples[index]);
        if (magnitude > bestMagnitude) {
            bestMagnitude = magnitude;
            bestIndex = index;
        }
    }
    return bestIndex;
}

std::vector<std::complex<double>> buildPositiveSpectrumFromTransfer(const std::vector<double>& frequencyAxisHz,
                                                                    const std::vector<double>& magnitudeDb,
                                                                    const std::vector<double>& phaseRadians,
                                                                    bool sourceOnLogAxis,
                                                                    int sampleRate,
                                                                    int fftSize) {
    const size_t positiveBinCount = static_cast<size_t>(std::max(fftSize / 2, 0) + 1);
    std::vector<std::complex<double>> positiveSpectrum;
    positiveSpectrum.reserve(positiveBinCount);
    for (size_t bin = 0; bin < positiveBinCount; ++bin) {
        const double frequencyHz = static_cast<double>(sampleRate) * static_cast<double>(bin) /
                                   static_cast<double>(std::max(fftSize, 1));
        const double magnitudeAtBinDb = frequencyHz <= 0.0
                                            ? magnitudeDb.front()
                                            : interpolateFrequency(frequencyAxisHz, magnitudeDb, frequencyHz, sourceOnLogAxis);
        const double phaseAtBinRadians = frequencyHz <= 0.0
                                             ? phaseRadians.front()
                                             : interpolateFrequency(frequencyAxisHz, phaseRadians, frequencyHz, sourceOnLogAxis);
        positiveSpectrum.push_back(std::polar(std::pow(10.0, magnitudeAtBinDb / 20.0), phaseAtBinRadians));
    }
    return positiveSpectrum;
}

std::vector<std::complex<double>> buildFullSpectrumFromPositive(const std::vector<std::complex<double>>& positiveSpectrum,
                                                                int fftSize) {
    const size_t spectrumSize = static_cast<size_t>(std::max(fftSize, 1));
    std::vector<std::complex<double>> spectrum(spectrumSize, {0.0, 0.0});
    const size_t positiveCount = std::min(positiveSpectrum.size(), (spectrumSize / 2) + 1);
    for (size_t index = 0; index < positiveCount; ++index) {
        spectrum[index] = positiveSpectrum[index];
    }
    for (size_t index = 1; index + 1 < positiveCount; ++index) {
        spectrum[spectrumSize - index] = std::conj(positiveSpectrum[index]);
    }
    return spectrum;
}

std::vector<double> buildImpulseFromPositiveSpectrum(const std::vector<std::complex<double>>& positiveSpectrum,
                                                     int fftSize) {
    std::vector<std::complex<double>> spectrum = buildFullSpectrumFromPositive(positiveSpectrum, fftSize);
    fft(spectrum, true);

    std::vector<double> impulse;
    impulse.reserve(spectrum.size());
    for (const std::complex<double>& sample : spectrum) {
        impulse.push_back(sample.real());
    }
    return impulse;
}

std::vector<double> extractCircularWindow(const std::vector<double>& impulse,
                                          size_t windowStart,
                                          size_t windowLength) {
    std::vector<double> window;
    if (impulse.empty() || windowLength == 0) {
        return window;
    }

    const size_t extractedLength = std::min(windowLength, impulse.size());
    window.reserve(extractedLength);
    for (size_t index = 0; index < extractedLength; ++index) {
        window.push_back(impulse[(windowStart + index) % impulse.size()]);
    }
    return window;
}

void applyTailCosineWindow(std::vector<double>& samples, size_t preRollFrames) {
    if (samples.size() < 16) {
        return;
    }

    const size_t fadeFrames = std::clamp(samples.size() / 16, size_t{8}, size_t{512});
    const size_t leadingFade = std::min(preRollFrames, fadeFrames);
    if (leadingFade > 1) {
        for (size_t index = 0; index < leadingFade; ++index) {
            const double t = static_cast<double>(index) / static_cast<double>(leadingFade - 1);
            const double weight = 0.5 - (0.5 * std::cos(std::numbers::pi_v<double> * t));
            samples[index] *= weight;
        }
    }

    if (fadeFrames > 1) {
        for (size_t index = 0; index < fadeFrames; ++index) {
            const double t = static_cast<double>(index) / static_cast<double>(fadeFrames - 1);
            const double weight = 0.5 * (1.0 + std::cos(std::numbers::pi_v<double> * t));
            samples[samples.size() - fadeFrames + index] *= weight;
        }
    }
}

WindowedTransferData applyExcessPhaseWindow(const std::vector<double>& frequencyAxisHz,
                                            const std::vector<double>& magnitudeDb,
                                            const std::vector<double>& delayCorrectedPhaseRadians,
                                            bool sourceOnLogAxis,
                                            int sampleRate,
                                            int fftSize,
                                            double excessPhaseWindowMs) {
    WindowedTransferData windowed;
    if (frequencyAxisHz.empty() || magnitudeDb.empty() || delayCorrectedPhaseRadians.empty()) {
        return windowed;
    }
    if (!std::isfinite(excessPhaseWindowMs) || excessPhaseWindowMs <= 0.0) {
        return windowed;
    }

    const std::vector<std::complex<double>> positiveSpectrum =
        buildPositiveSpectrumFromTransfer(frequencyAxisHz,
                                         magnitudeDb,
                                         delayCorrectedPhaseRadians,
                                         sourceOnLogAxis,
                                         sampleRate,
                                         fftSize);
    std::vector<double> impulse = buildImpulseFromPositiveSpectrum(positiveSpectrum, fftSize);
    if (impulse.empty()) {
        return windowed;
    }

    const size_t requestedWindowFrames = static_cast<size_t>(std::llround(
        excessPhaseWindowMs * static_cast<double>(std::max(sampleRate, 1)) / 1000.0));
    const size_t windowLength = std::min(requestedWindowFrames, impulse.size());
    if (windowLength < 16 || windowLength >= impulse.size()) {
        return windowed;
    }

    const size_t peakIndex = maxAbsIndex(impulse);
    const size_t preRollFrames = std::min<size_t>(windowLength / 8, 256);
    const size_t start = (peakIndex + impulse.size() - std::min(preRollFrames, impulse.size() - 1)) % impulse.size();
    std::vector<double> windowedImpulse = extractCircularWindow(impulse, start, windowLength);
    applyTailCosineWindow(windowedImpulse, preRollFrames);

    std::vector<std::complex<double>> windowedSpectrum(static_cast<size_t>(std::max(fftSize, 1)), {0.0, 0.0});
    for (size_t index = 0; index < windowedImpulse.size() && index < windowedSpectrum.size(); ++index) {
        windowedSpectrum[index] = {windowedImpulse[index], 0.0};
    }
    fft(windowedSpectrum, false);

    const std::vector<double> linearAxisHz = buildLinearFrequencyAxis(sampleRate, static_cast<size_t>(fftSize));
    const size_t positiveCount = std::min(linearAxisHz.size(), (windowedSpectrum.size() / 2) + 1);
    std::vector<double> windowedLinearMagnitudeDb;
    std::vector<double> windowedLinearPhaseRadians;
    windowedLinearMagnitudeDb.reserve(positiveCount);
    windowedLinearPhaseRadians.reserve(positiveCount);
    for (size_t index = 0; index < positiveCount; ++index) {
        windowedLinearMagnitudeDb.push_back(20.0 * std::log10(std::max(std::abs(windowedSpectrum[index]), 1.0e-9)));
        windowedLinearPhaseRadians.push_back(std::arg(windowedSpectrum[index]));
    }
    windowedLinearPhaseRadians = unwrapPhaseRadians(windowedLinearPhaseRadians);
    removeLinearDelay(windowedLinearPhaseRadians,
                      linearAxisHz,
                      static_cast<double>(preRollFrames) / static_cast<double>(std::max(sampleRate, 1)));

    windowed.frequencyAxisHz = frequencyAxisHz;
    windowed.magnitudeDb.reserve(frequencyAxisHz.size());
    windowed.phaseRadians.reserve(frequencyAxisHz.size());
    for (const double frequencyHz : frequencyAxisHz) {
        windowed.magnitudeDb.push_back(interpolateLinearFrequency(linearAxisHz,
                                                                  windowedLinearMagnitudeDb,
                                                                  frequencyHz));
        windowed.phaseRadians.push_back(interpolateLinearFrequency(linearAxisHz,
                                                                   windowedLinearPhaseRadians,
                                                                   frequencyHz));
    }
    if (!windowed.valid()) {
        return {};
    }
    return windowed;
}

std::vector<double> buildPositiveMagnitudeResponse(int sampleRate,
                                                   int fftSize,
                                                   const std::vector<double>& frequencyAxisHz,
                                                   const std::vector<double>& magnitudeDb) {
    const size_t positiveBinCount = static_cast<size_t>(std::max(fftSize / 2, 0) + 1);
    std::vector<double> magnitude;
    magnitude.reserve(positiveBinCount);
    for (size_t bin = 0; bin < positiveBinCount; ++bin) {
        const double frequencyHz = static_cast<double>(sampleRate) * static_cast<double>(bin) /
                                   static_cast<double>(std::max(fftSize, 1));
        const double magnitudeAtBinDb =
            frequencyHz <= 0.0 ? magnitudeDb.front() : interpolateLogFrequency(frequencyAxisHz, magnitudeDb, frequencyHz);
        magnitude.push_back(std::pow(10.0, magnitudeAtBinDb / 20.0));
    }
    return magnitude;
}

std::vector<std::complex<double>> buildMinimumPhaseSpectrum(const std::vector<double>& positiveMagnitude,
                                                            int fftSize) {
    const size_t spectrumSize = static_cast<size_t>(std::max(fftSize, 1));
    std::vector<std::complex<double>> logSpectrum(spectrumSize, {0.0, 0.0});
    const size_t positiveCount = std::min(positiveMagnitude.size(), (spectrumSize / 2) + 1);
    for (size_t index = 0; index < positiveCount; ++index) {
        const double logMagnitude = std::log(std::max(positiveMagnitude[index], 1.0e-9));
        logSpectrum[index] = {logMagnitude, 0.0};
    }
    for (size_t index = 1; index + 1 < positiveCount; ++index) {
        logSpectrum[spectrumSize - index] = logSpectrum[index];
    }

    std::vector<std::complex<double>> cepstrum = logSpectrum;
    fft(cepstrum, true);

    std::vector<std::complex<double>> minimumCepstrum(spectrumSize, {0.0, 0.0});
    if (!cepstrum.empty()) {
        minimumCepstrum[0] = cepstrum[0];
    }
    const size_t half = spectrumSize / 2;
    for (size_t index = 1; index < half; ++index) {
        minimumCepstrum[index] = cepstrum[index] * 2.0;
    }
    if ((spectrumSize % 2) == 0 && half < cepstrum.size()) {
        minimumCepstrum[half] = cepstrum[half];
    }

    std::vector<std::complex<double>> minimumLogSpectrum = minimumCepstrum;
    fft(minimumLogSpectrum, false);
    for (std::complex<double>& value : minimumLogSpectrum) {
        value = std::exp(value);
    }
    return minimumLogSpectrum;
}

std::vector<double> buildPhaseSeries(const std::vector<std::complex<double>>& spectrum) {
    const size_t positiveBinCount = (spectrum.size() / 2) + 1;
    std::vector<double> phase;
    phase.reserve(positiveBinCount);
    for (size_t index = 0; index < positiveBinCount; ++index) {
        phase.push_back(std::arg(spectrum[index]));
    }
    return unwrapPhaseRadians(phase);
}

std::vector<double> buildMinimumPhaseRadians(int sampleRate,
                                             int fftSize,
                                             const std::vector<double>& frequencyAxisHz,
                                             const std::vector<double>& magnitudeDb) {
    if (frequencyAxisHz.empty() || magnitudeDb.empty()) {
        return {};
    }

    const std::vector<double> positiveMagnitude =
        buildPositiveMagnitudeResponse(sampleRate, fftSize, frequencyAxisHz, magnitudeDb);
    const std::vector<std::complex<double>> minimumSpectrum =
        buildMinimumPhaseSpectrum(positiveMagnitude, fftSize);
    const std::vector<double> binFrequencyAxisHz = buildLinearFrequencyAxis(sampleRate, fftSize);
    const std::vector<double> binPhaseRadians = buildPhaseSeries(minimumSpectrum);
    return resampleLogFrequency(binFrequencyAxisHz, binPhaseRadians, frequencyAxisHz);
}

std::vector<double> buildGroupDelayMs(const std::vector<double>& frequencyAxisHz,
                                      const std::vector<double>& unwrappedPhaseRadians) {
    std::vector<double> groupDelayMs(frequencyAxisHz.size(), 0.0);
    if (frequencyAxisHz.size() < 3 || unwrappedPhaseRadians.size() < 3) {
        return groupDelayMs;
    }

    const size_t count = std::min(frequencyAxisHz.size(), unwrappedPhaseRadians.size());
    for (size_t index = 1; index + 1 < count; ++index) {
        const double phaseDelta = unwrappedPhaseRadians[index + 1] - unwrappedPhaseRadians[index - 1];
        const double frequencyDelta = frequencyAxisHz[index + 1] - frequencyAxisHz[index - 1];
        if (std::abs(frequencyDelta) < 1.0e-12) {
            continue;
        }
        groupDelayMs[index] = (-phaseDelta / (2.0 * std::numbers::pi_v<double> * frequencyDelta)) * 1000.0;
    }
    if (count >= 2) {
        groupDelayMs.front() = groupDelayMs[1];
        groupDelayMs[count - 1] = groupDelayMs[count - 2];
    }
    return groupDelayMs;
}

bool axesMatch(const MeasurementValueSet& left, const MeasurementValueSet& right) {
    if (left.xValues.size() != right.xValues.size()) {
        return false;
    }
    for (size_t index = 0; index < left.xValues.size(); ++index) {
        if (std::abs(left.xValues[index] - right.xValues[index]) > 1.0e-6) {
            return false;
        }
    }
    return true;
}

double bulkDelaySecondsFromImpulse(const MeasurementResult* result) {
    if (result == nullptr) {
        return 0.0;
    }

    const MeasurementValueSet* impulse = result->findValueSet("measurement.room_impulse_response");
    if (impulse != nullptr && impulse->valid() && !impulse->xValues.empty()) {
        return std::max(-impulse->xValues.front(), 0.0);
    }
    return 0.0;
}

struct PhaseSourceSelection {
    const MeasurementValueSet* magnitude = nullptr;
    const MeasurementValueSet* phase = nullptr;
    std::string sourceWindow;
    std::string sourceKey;
    std::string sourceSeriesKind;
    std::string magnitudeValueSetKey;
    std::string phaseValueSetKey;
};

PhaseSourceSelection selectPhaseSource(const MeasurementResult* result) {
    PhaseSourceSelection selection;
    if (result == nullptr) {
        return selection;
    }

    struct Candidate {
        std::string_view sourceWindow;
        std::string_view sourceKey;
        std::string_view sourceSeriesKind;
        std::string_view magnitudeKey;
        std::string_view phaseKey;
    };

    constexpr Candidate candidates[] = {
        {"room", "measurement.reference_compensated_room",
         "response",
         "measurement.reference_compensated_room_magnitude_response",
         "measurement.reference_compensated_room_phase_response"},
        {"room", "measurement.reference_compensated_room",
         "spectrum",
         "measurement.reference_compensated_room_magnitude_spectrum",
         "measurement.reference_compensated_room_phase_spectrum"},
        {"room", "measurement.room",
         "response",
         "measurement.room_magnitude_response",
         "measurement.room_phase_response"},
        {"room", "measurement.room",
         "spectrum",
         "measurement.room_magnitude_spectrum",
         "measurement.room_phase_spectrum"}
    };

    for (const Candidate& candidate : candidates) {
        const MeasurementValueSet* magnitude = result->findValueSet(candidate.magnitudeKey);
        const MeasurementValueSet* phase = result->findValueSet(candidate.phaseKey);
        if (magnitude == nullptr || phase == nullptr || !magnitude->valid() || !phase->valid()) {
            continue;
        }
        if (!axesMatch(*magnitude, *phase)) {
            continue;
        }

        selection.magnitude = magnitude;
        selection.phase = phase;
        selection.sourceWindow = std::string(candidate.sourceWindow);
        selection.sourceKey = std::string(candidate.sourceKey);
        selection.sourceSeriesKind = std::string(candidate.sourceSeriesKind);
        selection.magnitudeValueSetKey = std::string(candidate.magnitudeKey);
        selection.phaseValueSetKey = std::string(candidate.phaseKey);
        return selection;
    }

    return selection;
}

}  // namespace

PreparedPhaseChannel prepareMatchedPhaseChannel(const std::vector<double>& nativeFrequencyAxisHz,
                                                const std::vector<double>& measuredMagnitudeDb,
                                                const std::vector<double>& unwrappedPhaseRadians,
                                                double bulkDelaySeconds,
                                                const ResponseSmoothingSettings& smoothingSettings,
                                                int sampleRate,
                                                int fftSize,
                                                bool sourceOnLogAxis,
                                                double excessPhaseWindowMs,
                                                std::string sourceKey) {
    PreparedPhaseChannel channel;
    const size_t count = std::min({nativeFrequencyAxisHz.size(), measuredMagnitudeDb.size(), unwrappedPhaseRadians.size()});
    if (count < 3) {
        return channel;
    }

    channel.nativeFrequencyAxisHz.assign(nativeFrequencyAxisHz.begin(),
                                         nativeFrequencyAxisHz.begin() + static_cast<std::ptrdiff_t>(count));
    channel.measuredMagnitudeDb.assign(measuredMagnitudeDb.begin(),
                                       measuredMagnitudeDb.begin() + static_cast<std::ptrdiff_t>(count));
    channel.measuredPhaseRadians.assign(unwrappedPhaseRadians.begin(),
                                        unwrappedPhaseRadians.begin() + static_cast<std::ptrdiff_t>(count));
    channel.bulkDelaySeconds = std::max(bulkDelaySeconds, 0.0);
    channel.sourceKey = std::move(sourceKey);

    channel.delayCorrectedPhaseRadians = channel.measuredPhaseRadians;
    removeLinearDelay(channel.delayCorrectedPhaseRadians,
                      channel.nativeFrequencyAxisHz,
                      channel.bulkDelaySeconds);
    if (excessPhaseWindowMs > 0.0) {
        const WindowedTransferData windowed =
            applyExcessPhaseWindow(channel.nativeFrequencyAxisHz,
                                   channel.measuredMagnitudeDb,
                                   channel.delayCorrectedPhaseRadians,
                                   sourceOnLogAxis,
                                   sampleRate,
                                   fftSize,
                                   excessPhaseWindowMs);
        if (windowed.valid()) {
            channel.nativeFrequencyAxisHz = windowed.frequencyAxisHz;
            channel.measuredMagnitudeDb = windowed.magnitudeDb;
            channel.measuredPhaseRadians = windowed.phaseRadians;
            channel.delayCorrectedPhaseRadians = windowed.phaseRadians;
        }
    }
    channel.smoothedMagnitudeDb = smoothMagnitudeSeries(channel.nativeFrequencyAxisHz,
                                                        channel.measuredMagnitudeDb,
                                                        smoothingSettings);
    channel.minimumPhaseRadians = buildMinimumPhaseRadians(sampleRate,
                                                           fftSize,
                                                           channel.nativeFrequencyAxisHz,
                                                           channel.smoothedMagnitudeDb);
    channel.excessPhaseRadians =
        unwrapPhaseRadians(principalPhaseDifferenceRadians(channel.delayCorrectedPhaseRadians,
                                                           channel.minimumPhaseRadians));
    if (!channel.valid()) {
        return {};
    }
    return channel;
}

PreparedPhaseData preparePhaseData(const MeasurementResult* result,
                                   const ResponseSmoothingSettings& smoothingSettings,
                                   int sampleRate,
                                   int fftSize,
                                   double excessPhaseWindowMs) {
    PreparedPhaseData prepared;
    const PhaseSourceSelection selection = selectPhaseSource(result);
    if (selection.magnitude == nullptr || selection.phase == nullptr) {
        return prepared;
    }

    const double bulkDelaySeconds = bulkDelaySecondsFromImpulse(result);
    const bool sourceOnLogAxis = selection.sourceSeriesKind == "response";
    prepared.bulkDelaySeconds = bulkDelaySeconds;
    prepared.left = prepareMatchedPhaseChannel(selection.phase->xValues,
                                               selection.magnitude->leftValues,
                                               unwrapPhaseRadians(degreesToRadians(selection.phase->leftValues)),
                                               bulkDelaySeconds,
                                               smoothingSettings,
                                               sampleRate,
                                               fftSize,
                                               sourceOnLogAxis,
                                               excessPhaseWindowMs,
                                               selection.sourceKey);
    prepared.right = prepareMatchedPhaseChannel(selection.phase->xValues,
                                                selection.magnitude->rightValues,
                                                unwrapPhaseRadians(degreesToRadians(selection.phase->rightValues)),
                                                bulkDelaySeconds,
                                                smoothingSettings,
                                                sampleRate,
                                                fftSize,
                                                sourceOnLogAxis,
                                                excessPhaseWindowMs,
                                                selection.sourceKey);
    prepared.valid = prepared.left.valid() && prepared.right.valid();
    prepared.sourceWindow = selection.sourceWindow;
    prepared.sourceKey = selection.sourceKey;
    prepared.sourceSeriesKind = selection.sourceSeriesKind;
    prepared.magnitudeValueSetKey = selection.magnitudeValueSetKey;
    prepared.phaseValueSetKey = selection.phaseValueSetKey;
    return prepared;
}

PreparedPhaseView resamplePreparedPhaseChannel(const PreparedPhaseChannel& channel,
                                               const std::vector<double>& displayFrequencyAxisHz) {
    PreparedPhaseView view;
    if (!channel.valid() || displayFrequencyAxisHz.empty()) {
        return view;
    }

    view.frequencyAxisHz = displayFrequencyAxisHz;
    view.delayCorrectedPhaseRadians =
        resampleLogFrequency(channel.nativeFrequencyAxisHz,
                             channel.delayCorrectedPhaseRadians,
                             displayFrequencyAxisHz);
    view.minimumPhaseRadians =
        resampleLogFrequency(channel.nativeFrequencyAxisHz,
                             channel.minimumPhaseRadians,
                             displayFrequencyAxisHz);
    view.excessPhaseRadians =
        resampleLogFrequency(channel.nativeFrequencyAxisHz,
                             channel.excessPhaseRadians,
                             displayFrequencyAxisHz);
    view.wrappedExcessPhaseDegrees = wrapPhaseRadiansSeriesDegrees(view.excessPhaseRadians);
    view.continuousExcessPhaseDegrees = radiansToDegrees(view.excessPhaseRadians);
    view.groupDelayMs = buildGroupDelayMs(displayFrequencyAxisHz, view.delayCorrectedPhaseRadians);
    if (!view.valid()) {
        return {};
    }
    return view;
}

}  // namespace wolfie::measurement
