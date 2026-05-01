#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

#include "filter_test_support.h"
#include "test_harness.h"

#include "measurement/filter_designer.h"
#include "measurement/filter_wav_export.h"
#include "measurement/target_curve_designer.h"

namespace {

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

    const wolfie::SmoothedResponse response = wolfie::tests::buildSyntheticResponse();
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

    const wolfie::SmoothedResponse response = wolfie::tests::buildSyntheticResponse();
    const wolfie::MeasurementResult phaseMeasurement =
        wolfie::tests::buildPhaseMeasurement(measurement.sampleRate, 0.0, 3.0, 2.0);
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
        wolfie::tests::readFileBytes(wolfie::measurement::roonFilterWavPath(minimumDirectory, sampleRates.front()));
    const std::vector<char> mixedBytes =
        wolfie::tests::readFileBytes(wolfie::measurement::roonFilterWavPath(mixedDirectory, sampleRates.front()));
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

int main() {
    return wolfie::tests::runTestCases({
        {"expectRoonExportSupportsCommonSampleRates", expectRoonExportSupportsCommonSampleRates},
        {"expectRoonMixedExportDiffersFromMinimum", expectRoonMixedExportDiffersFromMinimum},
    });
}
