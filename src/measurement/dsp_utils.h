#pragma once

#include <complex>
#include <cstddef>
#include <vector>

namespace wolfie::measurement {

void fft(std::vector<std::complex<double>>& samples, bool inverse);
size_t nextPowerOfTwo(size_t value);
std::vector<double> unwrapPhaseRadians(const std::vector<double>& phaseRadians);
std::vector<double> buildLogFrequencyAxis(double minFrequencyHz, double maxFrequencyHz, size_t pointCount);
std::vector<double> buildLinearFrequencyAxis(int sampleRate, size_t fftSize);

}  // namespace wolfie::measurement
