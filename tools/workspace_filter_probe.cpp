#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include "measurement/filter_designer.h"
#include "measurement/response_smoother.h"
#include "persistence/workspace_repository.h"

namespace {

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
    const double t = std::clamp((x - x0) / std::max(x1 - x0, 1.0e-9), 0.0, 1.0);
    return values[lowerIndex] + ((values[upperIndex] - values[lowerIndex]) * t);
}

void printPoint(const std::string& label,
                const std::vector<double>& frequencyAxisHz,
                const std::vector<double>& values,
                double frequencyHz,
                const char* unit) {
    std::cout << label << " @ " << frequencyHz << " Hz = "
              << interpolateLogFrequency(frequencyAxisHz, values, frequencyHz) << " " << unit << "\n";
}

}  // namespace

int main(int argc, char** argv) {
    const std::filesystem::path workspacePath =
        argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path("workspaces/wozi");

    const wolfie::persistence::WorkspaceRepository repository;
    wolfie::WorkspaceState workspace = repository.load(workspacePath);
    if (!workspace.result.hasAnyValues()) {
        std::cerr << "workspace has no measurement result\n";
        return 1;
    }

    workspace.smoothedResponse = wolfie::measurement::buildSmoothedResponse(workspace.result, workspace.smoothing);
    const wolfie::FilterDesignResult rebuilt =
        wolfie::measurement::designFilters(workspace.smoothedResponse,
                                           workspace.measurement,
                                           workspace.targetCurve,
                                           workspace.filters,
                                           &workspace.result);
    if (!rebuilt.valid) {
        std::cerr << "rebuilt filter is invalid\n";
        return 1;
    }

    if (!workspace.minimumFilter.available()) {
        std::cerr << "workspace has no stored minimum filter\n";
        return 1;
    }

    const std::vector<double> probeFrequenciesHz = {180.0, 220.0, 240.0, 256.0, 270.0, 300.0, 340.0};
    std::cout << "workspace=" << workspacePath.string() << "\n";
    std::cout << "maxBoostDb=" << workspace.filters.maxBoostDb
              << ", smoothness=" << workspace.filters.smoothness
              << ", tapCount=" << workspace.filters.tapCount << "\n\n";

    for (const double frequencyHz : probeFrequenciesHz) {
        const double smoothedInputDb =
            interpolateLogFrequency(workspace.smoothedResponse.frequencyAxisHz,
                                    workspace.smoothedResponse.rightChannelDb,
                                    frequencyHz);
        const double targetDb =
            interpolateLogFrequency(rebuilt.frequencyAxisHz, rebuilt.targetCurveDb, frequencyHz);
        std::cout << "input @ " << frequencyHz << " Hz = " << smoothedInputDb << " dB\n";
        std::cout << "target @ " << frequencyHz << " Hz = " << targetDb << " dB\n";
        std::cout << "raw demand @ " << frequencyHz << " Hz = " << (targetDb - smoothedInputDb) << " dB\n";
        printPoint("stored correction", workspace.minimumFilter.result.frequencyAxisHz,
                   workspace.minimumFilter.result.right.correctionCurveDb, frequencyHz, "dB");
        printPoint("rebuilt correction", rebuilt.frequencyAxisHz, rebuilt.right.correctionCurveDb, frequencyHz, "dB");
        printPoint("stored predicted GD", workspace.minimumFilter.result.frequencyAxisHz,
                   workspace.minimumFilter.result.right.predictedGroupDelayMs, frequencyHz, "ms");
        printPoint("rebuilt predicted GD", rebuilt.frequencyAxisHz, rebuilt.right.predictedGroupDelayMs, frequencyHz, "ms");
        std::cout << "\n";
    }

    return 0;
}
