#include "measurement/target_curve_designer.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>

namespace wolfie::measurement {

namespace {

constexpr double kMinGraphFrequencyHz = 20.0;
constexpr double kMaxGraphFrequencyHz = 20000.0;
constexpr double kMinBellQ = 0.3;
constexpr double kMaxBellQ = 6.0;
constexpr double kMinBellGainDb = -12.0;
constexpr double kMaxBellGainDb = 12.0;
constexpr size_t kDefaultCurvePointCount = 256;

template <typename T>
T clampValue(T value, T low, T high) {
    return std::max(low, std::min(value, high));
}

double safeLogFrequency(double frequencyHz) {
    return std::log10(std::max(frequencyHz, 1e-6));
}

std::pair<double, double> resolveFrequencyBounds(const SmoothedResponse& response, const MeasurementSettings& measurement) {
    double minFrequencyHz = clampValue(measurement.startFrequencyHz, kMinGraphFrequencyHz, kMaxGraphFrequencyHz);
    double maxFrequencyHz = clampValue(measurement.endFrequencyHz, minFrequencyHz + 1.0, kMaxGraphFrequencyHz);
    if (!response.frequencyAxisHz.empty()) {
        minFrequencyHz = clampValue(std::max(minFrequencyHz, response.frequencyAxisHz.front()),
                                    kMinGraphFrequencyHz,
                                    kMaxGraphFrequencyHz);
        maxFrequencyHz = clampValue(std::min(maxFrequencyHz, response.frequencyAxisHz.back()),
                                    minFrequencyHz + 1.0,
                                    kMaxGraphFrequencyHz);
    }
    return {minFrequencyHz, maxFrequencyHz};
}

std::vector<double> buildDefaultFrequencyAxis(double minFrequencyHz, double maxFrequencyHz) {
    std::vector<double> axis;
    axis.reserve(kDefaultCurvePointCount);
    const double logMin = safeLogFrequency(minFrequencyHz);
    const double logMax = safeLogFrequency(maxFrequencyHz);
    for (size_t i = 0; i < kDefaultCurvePointCount; ++i) {
        const double t = kDefaultCurvePointCount == 1 ? 0.0 : static_cast<double>(i) / static_cast<double>(kDefaultCurvePointCount - 1);
        axis.push_back(std::pow(10.0, logMin + ((logMax - logMin) * t)));
    }
    return axis;
}

double interpolateLinear(double x, double x0, double y0, double x1, double y1) {
    if (std::abs(x1 - x0) < 1e-9) {
        return y1;
    }
    const double t = clampValue((x - x0) / (x1 - x0), 0.0, 1.0);
    return y0 + ((y1 - y0) * t);
}

double basicCurveValueDb(const TargetCurveSettings& settings, double frequencyHz, double minFrequencyHz, double maxFrequencyHz) {
    const double x = safeLogFrequency(frequencyHz);
    const double lowX = safeLogFrequency(minFrequencyHz);
    const double midX = safeLogFrequency(settings.midFrequencyHz);
    const double highX = safeLogFrequency(maxFrequencyHz);
    if (x <= midX) {
        return interpolateLinear(x, lowX, settings.lowGainDb, midX, settings.midGainDb);
    }
    return interpolateLinear(x, midX, settings.midGainDb, highX, settings.highGainDb);
}

double bandContributionDb(const TargetEqBand& band, int sampleRate, double frequencyHz) {
    const double centerHz = clampValue(band.frequencyHz, 1.0, frequencyHz * 1000.0);
    const double normalizedCenter = 2.0 * std::numbers::pi * centerHz / static_cast<double>(std::max(sampleRate, 1));
    const double a = std::pow(10.0, band.gainDb / 40.0);
    const double alpha = std::sin(normalizedCenter) / (2.0 * clampValue(band.q, kMinBellQ, kMaxBellQ));

    const double b0 = 1.0 + (alpha * a);
    const double b1 = -2.0 * std::cos(normalizedCenter);
    const double b2 = 1.0 - (alpha * a);
    const double a0 = 1.0 + (alpha / a);
    const double a1 = -2.0 * std::cos(normalizedCenter);
    const double a2 = 1.0 - (alpha / a);

    const double omega = 2.0 * std::numbers::pi * clampValue(frequencyHz, 1.0, kMaxGraphFrequencyHz) /
                         static_cast<double>(std::max(sampleRate, 1));
    const std::complex<double> z1 = std::exp(std::complex<double>(0.0, -omega));
    const std::complex<double> z2 = z1 * z1;
    const std::complex<double> numerator = b0 + (b1 * z1) + (b2 * z2);
    const std::complex<double> denominator = a0 + (a1 * z1) + (a2 * z2);
    const double magnitude = std::abs(numerator / denominator);
    return 20.0 * std::log10(std::max(magnitude, 1e-9));
}

}  // namespace

TargetEqBand makeDefaultTargetEqBand(double frequencyHz, int colorIndex) {
    TargetEqBand band;
    band.enabled = false;
    band.colorIndex = colorIndex;
    band.frequencyHz = frequencyHz;
    band.gainDb = 0.0;
    band.q = 1.0;
    return band;
}

std::vector<TargetEqBand> defaultTargetEqBands() {
    return {
        makeDefaultTargetEqBand(200.0, 0),
        makeDefaultTargetEqBand(500.0, 1),
        makeDefaultTargetEqBand(1000.0, 2),
        makeDefaultTargetEqBand(5000.0, 3),
    };
}

void normalizeTargetCurveSettings(TargetCurveSettings& settings, double minFrequencyHz, double maxFrequencyHz) {
    minFrequencyHz = clampValue(minFrequencyHz, kMinGraphFrequencyHz, kMaxGraphFrequencyHz);
    maxFrequencyHz = clampValue(maxFrequencyHz, minFrequencyHz + 1.0, kMaxGraphFrequencyHz);
    if (settings.eqBands.empty()) {
        settings.eqBands = defaultTargetEqBands();
    }

    settings.midFrequencyHz = clampValue(settings.midFrequencyHz, minFrequencyHz, maxFrequencyHz);
    for (size_t i = 0; i < settings.eqBands.size(); ++i) {
        TargetEqBand& band = settings.eqBands[i];
        band.colorIndex = band.colorIndex < 0 ? static_cast<int>(i) : band.colorIndex;
        band.frequencyHz = clampValue(band.frequencyHz, minFrequencyHz, maxFrequencyHz);
        band.gainDb = clampValue(band.gainDb, kMinBellGainDb, kMaxBellGainDb);
        band.q = clampValue(band.q, kMinBellQ, kMaxBellQ);
    }
}

double evaluateTargetCurveDbAtFrequency(const MeasurementSettings& measurement,
                                        const TargetCurveSettings& sourceSettings,
                                        double minFrequencyHz,
                                        double maxFrequencyHz,
                                        double frequencyHz) {
    TargetCurveSettings settings = sourceSettings;
    normalizeTargetCurveSettings(settings, minFrequencyHz, maxFrequencyHz);

    const double clampedFrequencyHz = clampValue(frequencyHz, minFrequencyHz, maxFrequencyHz);
    const double basicDb = basicCurveValueDb(settings, clampedFrequencyHz, minFrequencyHz, maxFrequencyHz);
    if (settings.bypassEqBands) {
        return basicDb;
    }

    double eqDb = 0.0;
    for (const TargetEqBand& band : settings.eqBands) {
        if (!band.enabled) {
            continue;
        }
        eqDb += bandContributionDb(band, measurement.sampleRate, clampedFrequencyHz);
    }
    return basicDb + eqDb;
}

TargetCurvePlotData buildTargetCurvePlotData(const SmoothedResponse& response,
                                             const MeasurementSettings& measurement,
                                             const TargetCurveSettings& sourceSettings,
                                             std::optional<size_t> selectedBandIndex) {
    const auto [minFrequencyHz, maxFrequencyHz] = resolveFrequencyBounds(response, measurement);
    TargetCurveSettings settings = sourceSettings;
    normalizeTargetCurveSettings(settings, minFrequencyHz, maxFrequencyHz);

    TargetCurvePlotData plot;
    plot.minFrequencyHz = minFrequencyHz;
    plot.maxFrequencyHz = maxFrequencyHz;
    plot.frequencyAxisHz = response.frequencyAxisHz.empty() ? buildDefaultFrequencyAxis(minFrequencyHz, maxFrequencyHz)
                                                            : response.frequencyAxisHz;
    plot.basicCurveDb.reserve(plot.frequencyAxisHz.size());
    plot.targetCurveDb.reserve(plot.frequencyAxisHz.size());
    const bool includeEqBands = !settings.bypassEqBands;
    if (selectedBandIndex && *selectedBandIndex < settings.eqBands.size()) {
        plot.selectedBandContributionDb.reserve(plot.frequencyAxisHz.size());
    }

    for (size_t i = 0; i < plot.frequencyAxisHz.size(); ++i) {
        const double frequencyHz = clampValue(plot.frequencyAxisHz[i], minFrequencyHz, maxFrequencyHz);
        const double basicDb = basicCurveValueDb(settings, frequencyHz, minFrequencyHz, maxFrequencyHz);
        plot.basicCurveDb.push_back(basicDb);

        double eqDb = 0.0;
        double selectedDb = 0.0;
        for (size_t bandIndex = 0; bandIndex < settings.eqBands.size(); ++bandIndex) {
            const TargetEqBand& band = settings.eqBands[bandIndex];
            if (!includeEqBands || !band.enabled) {
                continue;
            }
            const double contributionDb = bandContributionDb(band, measurement.sampleRate, frequencyHz);
            eqDb += contributionDb;
            if (selectedBandIndex && bandIndex == *selectedBandIndex) {
                selectedDb = contributionDb;
            }
        }

        plot.targetCurveDb.push_back(basicDb + eqDb);
        if (selectedBandIndex && *selectedBandIndex < settings.eqBands.size()) {
            plot.selectedBandContributionDb.push_back(selectedDb);
        }
    }

    return plot;
}

}  // namespace wolfie::measurement
