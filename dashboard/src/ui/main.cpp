#include <cstdio>
#include <cstdlib>

#include <chrono>
#include <string>

#include "ui/DashboardApp.h"
#include "app/BuildInfo.h"
#include "util/log/Logger.h"

namespace {

constexpr int EXIT_USAGE = 2;
constexpr long long POLL_MIN_MS = 200;
constexpr long long POLL_MAX_MS = 10000;
constexpr long long PREVIEW_FPS_MIN = 1;
constexpr long long PREVIEW_FPS_MAX = 30;
constexpr long long MILLIS_PER_SECOND = 1000;

void printUsage() {
    std::printf("usage: synccap_dashboard [--server http://127.0.0.1:8080] [--poll-ms 1000]\n"
                "                       [--preview http://127.0.0.1:8090] [--preview-fps 10] [--no-preview]\n");
}

}  // namespace

int main(int argc, char** argv) {
    dashboard_options options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        const bool hasValue = index + 1 < argc;
        if (argument == "--server" && hasValue) {
            options.serverUrl = argv[++index];
        } else if (argument == "--poll-ms" && hasValue) {
            const long long pollMs = std::atoll(argv[++index]);
            if (pollMs < POLL_MIN_MS || pollMs > POLL_MAX_MS) {
                printUsage();
                return EXIT_USAGE;
            }
            options.pollPeriod = std::chrono::milliseconds(pollMs);
        } else if (argument == "--preview" && hasValue) {
            options.previewUrl = argv[++index];
        } else if (argument == "--preview-fps" && hasValue) {
            const long long fps = std::atoll(argv[++index]);
            if (fps < PREVIEW_FPS_MIN || fps > PREVIEW_FPS_MAX) {
                printUsage();
                return EXIT_USAGE;
            }
            options.previewPeriod = std::chrono::milliseconds(MILLIS_PER_SECOND / fps);
        } else if (argument == "--no-preview") {
            options.previewEnabled = false;
        } else {
            printUsage();
            return EXIT_USAGE;
        }
    }

    log_options logOptions;
    logOptions.directory = "logs-dashboard";
    if (!Logger::initialize(logOptions)) {
        return EXIT_USAGE;
    }
    LOG_INFO(app, life, "starting dashboard version={} commit={} build={}", BUILD_VERSION, BUILD_COMMIT, BUILD_TYPE);
    int exitCode = 0;
    {
        DashboardApp app(options);
        exitCode = app.run();
    }
    Logger::shutdown();
    return exitCode;
}
