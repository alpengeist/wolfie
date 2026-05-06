#include "measurement/waterfall_builder.h"

#include <algorithm>
#include <cmath>
#include <complex>

#include "measurement/dsp_utils.h"
#include "measurement/response_smoother.h"

namespace wolfie::measurement {

namespace {

template <typename T>
T clampValue(T value, T low, T high) {
    return std::max(low, std::min(value, high));
}

constexpr double kPi = 3.14159265358979323846;
constexpr double kWaterfallSliceHopMs = 8.0;
constexpr double kWaterfallMaxDurationMs = 3000.0;
constexpr size_t kWaterfallDisplayFrequencyPointCount = 512;
constexpr size_t kWaterfallSmoothingFrequencyPointCount = 2048;

std::vector<double> smoothWaterfallSliceDb(const std::vector<double>& frequencyAxisHz,
                                           const std::vector<double>& sliceValuesDb) {
    if (frequencyAxisHz.empty() || sliceValuesDb.size() != frequencyAxisHz.size()) {
        return sliceValuesDb;
    }

    ResponseSmoothingSettings settings;
    settings.psychoacousticModel = "octave sliding window";
    settings.resolutionPercent = 100;
    settings.highFrequencySlopeCutoffHz = frequencyAxisHz.back();
    return smoothMagnitudeSeries(frequencyAxisHz, sliceValuesDb, settings);
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
    if (upperIndex == 0) {
        return values.front();
    }

    const size_t lowerIndex = upperIndex - 1;
    const double x = std::log10(std::max(frequencyHz, 1.0));
    const double x0 = std::log10(std::max(frequencyAxisHz[lowerIndex], 1.0));
    const double x1 = std::log10(std::max(frequencyAxisHz[upperIndex], 1.0));
    const double blend = clampValue((x - x0) / std::max(x1 - x0, 1.0e-12), 0.0, 1.0);
    return values[lowerIndex] + ((values[upperIndex] - values[lowerIndex]) * blend);
}

std::vector<double> resampleLogFrequency(const std::vector<double>& sourceFrequencyAxisHz,
                                         const std::vector<double>& sourceValues,
                                         const std::vector<double>& targetFrequencyAxisHz) {
    std::vector<double> resampled;
    resampled.reserve(targetFrequencyAxisHz.size());
    for (const double frequencyHz : targetFrequencyAxisHz) {
        resampled.push_back(interpolateLogFrequency(sourceFrequencyAxisHz, sourceValues, frequencyHz));
    }
    return resampled;
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

std::vector<double> convolveAndAlignImpulse(const std::vector<double>& impulse,
                                            const std::vector<double>& filterTaps,
                                            int filterPeakIndex) {
    if (impulse.empty() || filterTaps.empty()) {
        return {};
    }

    const size_t outputLength = impulse.size() + filterTaps.size() - 1;
    const size_t fftSize = nextPowerOfTwo(outputLength);
    std::vector<std::complex<double>> impulseSpectrum(fftSize, std::complex<double>(0.0, 0.0));
    std::vector<std::complex<double>> filterSpectrum(fftSize, std::complex<double>(0.0, 0.0));
    for (size_t index = 0; index < impulse.size(); ++index) {
        impulseSpectrum[index] = std::complex<double>(impulse[index], 0.0);
    }
    for (size_t index = 0; index < filterTaps.size(); ++index) {
        filterSpectrum[index] = std::complex<double>(filterTaps[index], 0.0);
    }

    fft(impulseSpectrum, false);
    fft(filterSpectrum, false);
    for (size_t index = 0; index < fftSize; ++index) {
        impulseSpectrum[index] *= filterSpectrum[index];
    }
    fft(impulseSpectrum, true);

    std::vector<double> convolved;
    convolved.reserve(outputLength);
    for (size_t index = 0; index < outputLength; ++index) {
        convolved.push_back(impulseSpectrum[index].real());
    }

    const size_t alignmentSamples = static_cast<size_t>(std::max(filterPeakIndex, 0));
    if (alignmentSamples >= convolved.size()) {
        return {};
    }

    return std::vector<double>(convolved.begin() + static_cast<std::ptrdiff_t>(alignmentSamples), convolved.end());
}

WaterfallPlotData buildWaterfallPlotDataFromImpulse(const MeasurementResult& result,
                                                    const MeasurementValueSet& impulse,
                                                    const std::vector<double>& values) {
    WaterfallPlotData plot;
    if (!impulse.valid()) {
        return plot;
    }

    if (values.empty()) {
        return plot;
    }
    if (impulse.xValues.size() > values.size()) {
        return plot;
    }

    const int sampleRate = inferSampleRate(result, impulse);
    if (sampleRate <= 0) {
        return plot;
    }

    const size_t startIndex = zeroTimeIndex(impulse);
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

    plot.frequencyAxisHz = buildLogFrequencyAxis(20.0, maxFrequencyHz, kWaterfallDisplayFrequencyPointCount);
    if (plot.frequencyAxisHz.empty()) {
        return plot;
    }
    const std::vector<double> smoothingFrequencyAxisHz =
        buildLogFrequencyAxis(20.0, maxFrequencyHz, kWaterfallSmoothingFrequencyPointCount);
    if (smoothingFrequencyAxisHz.empty()) {
        return plot;
    }

    const double availableDurationMs =
        static_cast<double>(positiveImpulse.size()) * 1000.0 / static_cast<double>(sampleRate);
    const double maxSliceDurationMs = std::min(kWaterfallMaxDurationMs, availableDurationMs);
    const int sliceCount =
        clampValue(static_cast<int>(std::floor(maxSliceDurationMs / kWaterfallSliceHopMs)) + 1, 24, 256);
    const size_t maxStartSample =
        positiveImpulse.size() > windowLength ? positiveImpulse.size() - windowLength : 0;
    const size_t maxDisplayedStartSample = std::min(
        maxStartSample,
        static_cast<size_t>(std::lround((maxSliceDurationMs * static_cast<double>(sampleRate)) / 1000.0)));

    std::vector<std::vector<double>> linearMagnitudes;
    linearMagnitudes.reserve(static_cast<size_t>(sliceCount));
    double globalPeakMagnitude = 0.0;

    for (int sliceIndex = 0; sliceIndex < sliceCount; ++sliceIndex) {
        const double sliceT = sliceCount <= 1
                                  ? 0.0
                                  : static_cast<double>(sliceIndex) / static_cast<double>(sliceCount - 1);
        const size_t startSample =
            static_cast<size_t>(std::lround(sliceT * static_cast<double>(maxDisplayedStartSample)));
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
        sliceMagnitudes.reserve(smoothingFrequencyAxisHz.size());
        for (const double frequencyHz : smoothingFrequencyAxisHz) {
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
        std::vector<double> sliceValuesDb;
        sliceValuesDb.reserve(linearMagnitudes[i].size());
        for (const double magnitude : linearMagnitudes[i]) {
            const double relativeMagnitude = std::max(magnitude, 1.0e-12) / globalPeakMagnitude;
            const double valueDb = 20.0 * std::log10(relativeMagnitude);
            sliceValuesDb.push_back(clampValue(valueDb, plot.minDb, plot.maxDb));
        }
        const std::vector<double> smoothedSliceValuesDb =
            smoothWaterfallSliceDb(smoothingFrequencyAxisHz, sliceValuesDb);
        plot.slices[i].valuesDb =
            resampleLogFrequency(smoothingFrequencyAxisHz, smoothedSliceValuesDb, plot.frequencyAxisHz);
    }

    return plot.valid() ? plot : WaterfallPlotData{};
}

}  // namespace

WaterfallPlotData buildWaterfallPlotData(const MeasurementResult& result, MeasurementChannel channel) {
    const MeasurementValueSet* impulse = result.findValueSet("measurement.raw_impulse_response");
    if (impulse == nullptr || !impulse->valid()) {
        return {};
    }

    const std::vector<double>& values =
        channel == MeasurementChannel::Right ? impulse->rightValues : impulse->leftValues;
    return buildWaterfallPlotDataFromImpulse(result, *impulse, values);
}

WaterfallPlotData buildExpectedWaterfallPlotData(const MeasurementResult& result,
                                                 const FilterDesignResult& filterResult,
                                                 MeasurementChannel channel) {
    const MeasurementValueSet* impulse = result.findValueSet("measurement.raw_impulse_response");
    if (impulse == nullptr || !impulse->valid() || !filterResult.valid) {
        return {};
    }

    const FilterDesignChannelResult& filterChannel =
        channel == MeasurementChannel::Right ? filterResult.right : filterResult.left;
    if (filterChannel.filterTaps.empty()) {
        return {};
    }

    const std::vector<double>& sourceValues =
        channel == MeasurementChannel::Right ? impulse->rightValues : impulse->leftValues;
    const std::vector<double> expectedValues =
        convolveAndAlignImpulse(sourceValues, filterChannel.filterTaps, filterChannel.impulsePeakIndex);
    if (expectedValues.empty()) {
        return {};
    }

    return buildWaterfallPlotDataFromImpulse(result, *impulse, expectedValues);
}

}  // namespace wolfie::measurement
