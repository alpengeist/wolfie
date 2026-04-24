#include "measurement/waterfall_builder.h"

#include <algorithm>
#include <cmath>
#include <complex>

namespace wolfie::measurement {

namespace {

template <typename T>
T clampValue(T value, T low, T high) {
    return std::max(low, std::min(value, high));
}

constexpr double kPi = 3.14159265358979323846;

size_t nextPowerOfTwo(size_t value) {
    size_t power = 1;
    while (power < value) {
        power <<= 1U;
    }
    return power;
}

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
        const double angle = (inverse ? 2.0 : -2.0) * kPi / static_cast<double>(length);
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

std::vector<double> buildLogFrequencyAxis(double minFrequencyHz, double maxFrequencyHz, size_t pointCount) {
    std::vector<double> axis;
    if (pointCount == 0 || maxFrequencyHz <= minFrequencyHz) {
        return axis;
    }

    const double logMin = std::log10(minFrequencyHz);
    const double logMax = std::log10(maxFrequencyHz);
    axis.reserve(pointCount);
    for (size_t i = 0; i < pointCount; ++i) {
        const double t = pointCount <= 1 ? 0.0 : static_cast<double>(i) / static_cast<double>(pointCount - 1);
        axis.push_back(std::pow(10.0, logMin + ((logMax - logMin) * t)));
    }
    return axis;
}

std::complex<double> interpolateSpectrumAtFrequency(const std::vector<std::complex<double>>& spectrum,
                                                    int sampleRate,
                                                    double frequencyHz) {
    if (spectrum.empty()) {
        return {};
    }

    const size_t positiveBinCount = (spectrum.size() / 2) + 1;
    const double maxFrequencyHz = static_cast<double>(std::max(sampleRate, 1)) * 0.5;
    const double clampedFrequencyHz = clampValue(frequencyHz, 0.0, maxFrequencyHz);
    const double scaledBin = clampedFrequencyHz * static_cast<double>(spectrum.size()) /
                             static_cast<double>(std::max(sampleRate, 1));
    const size_t lowIndex = clampValue<size_t>(static_cast<size_t>(scaledBin), 0, positiveBinCount - 1);
    const size_t highIndex = clampValue<size_t>(lowIndex + 1, 0, positiveBinCount - 1);
    if (highIndex == lowIndex) {
        return spectrum[lowIndex];
    }

    const double blend = clampValue(scaledBin - static_cast<double>(lowIndex), 0.0, 1.0);
    return spectrum[lowIndex] + ((spectrum[highIndex] - spectrum[lowIndex]) * blend);
}

int inferSampleRate(const MeasurementResult& result, const MeasurementValueSet& impulse) {
    if (result.analysis.sampleRate > 0) {
        return result.analysis.sampleRate;
    }
    if (impulse.xValues.size() < 2) {
        return 0;
    }

    const double deltaSeconds = impulse.xValues[1] - impulse.xValues[0];
    if (deltaSeconds <= 1.0e-9) {
        return 0;
    }
    return static_cast<int>(std::lround(1.0 / deltaSeconds));
}

size_t zeroTimeIndex(const MeasurementValueSet& impulse) {
    if (impulse.xValues.empty()) {
        return 0;
    }

    for (size_t i = 0; i < impulse.xValues.size(); ++i) {
        if (impulse.xValues[i] >= 0.0) {
            return i;
        }
    }
    return impulse.xValues.size() - 1;
}

void applyHannWindow(std::vector<double>& samples) {
    if (samples.size() < 2) {
        return;
    }

    const double denominator = static_cast<double>(samples.size() - 1);
    for (size_t i = 0; i < samples.size(); ++i) {
        const double weight = 0.5 - (0.5 * std::cos((2.0 * kPi * static_cast<double>(i)) / denominator));
        samples[i] *= weight;
    }
}

std::vector<std::complex<double>> fftOfRealSignal(const std::vector<double>& samples, size_t fftSize) {
    std::vector<std::complex<double>> spectrum(fftSize, std::complex<double>(0.0, 0.0));
    for (size_t i = 0; i < samples.size() && i < fftSize; ++i) {
        spectrum[i] = std::complex<double>(samples[i], 0.0);
    }
    fft(spectrum, false);
    return spectrum;
}

}  // namespace

WaterfallPlotData buildWaterfallPlotData(const MeasurementResult& result, MeasurementChannel channel) {
    WaterfallPlotData plot;
    const MeasurementValueSet* impulse = result.findValueSet("measurement.raw_impulse_response");
    if (impulse == nullptr || !impulse->valid()) {
        return plot;
    }

    const std::vector<double>& values =
        channel == MeasurementChannel::Right ? impulse->rightValues : impulse->leftValues;
    if (values.size() != impulse->xValues.size()) {
        return plot;
    }

    const int sampleRate = inferSampleRate(result, *impulse);
    if (sampleRate <= 0) {
        return plot;
    }

    const size_t startIndex = zeroTimeIndex(*impulse);
    if (startIndex >= values.size()) {
        return plot;
    }

    const std::vector<double> positiveImpulse(values.begin() + static_cast<std::ptrdiff_t>(startIndex), values.end());
    if (positiveImpulse.size() < 512) {
        return plot;
    }

    const size_t preferredWindowLength = clampValue<size_t>(nextPowerOfTwo(static_cast<size_t>(sampleRate / 8)),
                                                            2048,
                                                            8192);
    const size_t windowLength = std::min(preferredWindowLength, positiveImpulse.size());
    if (windowLength < 512) {
        return plot;
    }
    const size_t fftSize = nextPowerOfTwo(windowLength);

    const double maxFrequencyHz = std::min(20000.0, static_cast<double>(sampleRate) * 0.5);
    if (maxFrequencyHz <= 20.0) {
        return plot;
    }

    plot.frequencyAxisHz = buildLogFrequencyAxis(20.0, maxFrequencyHz, 160);
    if (plot.frequencyAxisHz.empty()) {
        return plot;
    }

    const double availableDurationMs =
        static_cast<double>(positiveImpulse.size()) * 1000.0 / static_cast<double>(sampleRate);
    const double maxSliceDurationMs = std::min(400.0, availableDurationMs);
    const int sliceCount = clampValue(static_cast<int>(std::floor(maxSliceDurationMs / 16.0)), 14, 24);
    const size_t maxStartSample =
        positiveImpulse.size() > windowLength ? positiveImpulse.size() - windowLength : 0;

    std::vector<std::vector<double>> linearMagnitudes;
    linearMagnitudes.reserve(static_cast<size_t>(sliceCount));
    double globalPeakMagnitude = 0.0;

    for (int sliceIndex = 0; sliceIndex < sliceCount; ++sliceIndex) {
        const double sliceT = sliceCount <= 1
                                  ? 0.0
                                  : static_cast<double>(sliceIndex) / static_cast<double>(sliceCount - 1);
        const size_t startSample = static_cast<size_t>(std::lround(sliceT * static_cast<double>(maxStartSample)));
        std::vector<double> windowed(windowLength, 0.0);
        std::copy_n(positiveImpulse.begin() + static_cast<std::ptrdiff_t>(startSample),
                    windowLength,
                    windowed.begin());
        applyHannWindow(windowed);

        const std::vector<std::complex<double>> spectrum = fftOfRealSignal(windowed, fftSize);
        if (spectrum.empty()) {
            continue;
        }

        std::vector<double> sliceMagnitudes;
        sliceMagnitudes.reserve(plot.frequencyAxisHz.size());
        for (const double frequencyHz : plot.frequencyAxisHz) {
            const double magnitude = std::abs(interpolateSpectrumAtFrequency(spectrum, sampleRate, frequencyHz));
            sliceMagnitudes.push_back(magnitude);
            globalPeakMagnitude = std::max(globalPeakMagnitude, magnitude);
        }
        linearMagnitudes.push_back(std::move(sliceMagnitudes));

        WaterfallSlice slice;
        slice.timeMilliseconds =
            static_cast<double>(startSample) * 1000.0 / static_cast<double>(sampleRate);
        plot.slices.push_back(std::move(slice));
    }

    if (plot.slices.empty() || globalPeakMagnitude <= 1.0e-12) {
        return {};
    }

    for (size_t i = 0; i < plot.slices.size() && i < linearMagnitudes.size(); ++i) {
        plot.slices[i].valuesDb.reserve(linearMagnitudes[i].size());
        for (const double magnitude : linearMagnitudes[i]) {
            const double relativeMagnitude = std::max(magnitude, 1.0e-12) / globalPeakMagnitude;
            const double valueDb = 20.0 * std::log10(relativeMagnitude);
            plot.slices[i].valuesDb.push_back(clampValue(valueDb, plot.minDb, plot.maxDb));
        }
    }

    return plot.valid() ? plot : WaterfallPlotData{};
}

}  // namespace wolfie::measurement
