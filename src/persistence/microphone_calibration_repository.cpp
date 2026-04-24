#include "persistence/microphone_calibration_repository.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <utility>
#include <vector>

namespace wolfie::persistence {

namespace {

void clearLoadedCalibration(AudioSettings& settings) {
    settings.microphoneCalibrationFrequencyHz.clear();
    settings.microphoneCalibrationCorrectionDb.clear();
}

std::string normalizeCalibrationLine(std::string line) {
    if (line.size() >= 3 &&
        static_cast<unsigned char>(line[0]) == 0xEF &&
        static_cast<unsigned char>(line[1]) == 0xBB &&
        static_cast<unsigned char>(line[2]) == 0xBF) {
        line.erase(0, 3);
    }

    for (char& ch : line) {
        if (ch == ',' || ch == ';' || ch == '\t') {
            ch = ' ';
        }
    }
    return line;
}

bool isFinitePositive(double value) {
    return std::isfinite(value) && value > 0.0;
}

bool isFiniteValue(double value) {
    return std::isfinite(value);
}

}  // namespace

bool loadMicrophoneCalibration(AudioSettings& settings, std::wstring& errorMessage) {
    clearLoadedCalibration(settings);
    errorMessage.clear();

    if (settings.microphoneCalibrationPath.empty()) {
        return true;
    }

    std::ifstream in(settings.microphoneCalibrationPath, std::ios::binary);
    if (!in) {
        errorMessage = L"Could not open microphone calibration file: " + settings.microphoneCalibrationPath.wstring();
        return false;
    }

    std::vector<std::pair<double, double>> points;
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream row(normalizeCalibrationLine(std::move(line)));
        double frequencyHz = 0.0;
        double correctionDb = 0.0;
        if (!(row >> frequencyHz >> correctionDb)) {
            continue;
        }
        if (!isFinitePositive(frequencyHz) || !isFiniteValue(correctionDb)) {
            continue;
        }
        points.emplace_back(frequencyHz, correctionDb);
    }

    if (points.size() < 2) {
        errorMessage = L"Microphone calibration file did not contain at least two valid frequency/correction rows.";
        return false;
    }

    std::sort(points.begin(), points.end(), [](const auto& left, const auto& right) {
        return left.first < right.first;
    });

    settings.microphoneCalibrationFrequencyHz.reserve(points.size());
    settings.microphoneCalibrationCorrectionDb.reserve(points.size());
    for (const auto& [frequencyHz, correctionDb] : points) {
        if (!settings.microphoneCalibrationFrequencyHz.empty() &&
            std::abs(settings.microphoneCalibrationFrequencyHz.back() - frequencyHz) < 1.0e-9) {
            settings.microphoneCalibrationCorrectionDb.back() = correctionDb;
            continue;
        }

        settings.microphoneCalibrationFrequencyHz.push_back(frequencyHz);
        settings.microphoneCalibrationCorrectionDb.push_back(correctionDb);
    }

    if (settings.microphoneCalibrationFrequencyHz.size() < 2) {
        clearLoadedCalibration(settings);
        errorMessage = L"Microphone calibration file collapsed to fewer than two unique frequency rows.";
        return false;
    }

    return true;
}

}  // namespace wolfie::persistence
