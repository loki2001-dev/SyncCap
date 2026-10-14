#include <cstdio>
#include <cstdlib>

#include <chrono>
#include <string>

#include "app/Application.h"
#include "app/BuildInfo.h"
#include "codec/FfmpegLog.h"
#include "config/ConfigLoader.h"
#include "util/log/Logger.h"

namespace {

constexpr int EXIT_USAGE = 2;
constexpr int EXIT_CONFIG = 2;

struct command_line {
    std::string configPath = "config/collector.json";
    long long durationS = 0;
    std::string logLevel;
};

void printUsage() {
    std::printf("usage: synccap_collector [--config path] [--duration seconds] [--log-level debug|info|warn|error]\n");
}

bool parseArguments(int argc, char** argv, command_line& options) {
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        const bool hasValue = index + 1 < argc;
        if (argument == "--config" && hasValue) {
            options.configPath = argv[++index];
        } else if (argument == "--duration" && hasValue) {
            options.durationS = std::atoll(argv[++index]);
        } else if (argument == "--log-level" && hasValue) {
            options.logLevel = argv[++index];
        } else {
            return false;
        }
    }
    return options.durationS >= 0;
}

}  // namespace

int main(int argc, char** argv) {
    command_line options;
    if (!parseArguments(argc, argv, options)) {
        printUsage();
        return EXIT_USAGE;
    }

    app_config config;
    config_issues issues;
    const bool loaded = ConfigLoader::loadFile(options.configPath, config, issues);
    if (!options.logLevel.empty()) {
        config.log.level = options.logLevel;
    }

    log_options logOptions;
    logOptions.directory = config.log.directory;
    logOptions.level = config.log.level;
    if (!Logger::initialize(logOptions)) {
        return EXIT_CONFIG;
    }
    FfmpegLog::install();

    LOG_INFO(app, life, "starting version={} commit={} build={} config={}", BUILD_VERSION, BUILD_COMMIT, BUILD_TYPE,
             options.configPath);
    for (const std::string& warning : issues.warnings) {
        LOG_WARN(app, cfg, "{}", warning);
    }
    if (!loaded) {
        for (const std::string& error : issues.errors) {
            LOG_ERROR(app, cfg, "{}", error);
        }
        LOG_ERROR(app, cfg, "refusing to run errors={}", issues.errors.size());
        Logger::shutdown();
        return EXIT_CONFIG;
    }
    for (const std::string& line : ConfigLoader::summarize(config)) {
        LOG_INFO(app, cfg, "{}", line);
    }

    int exitCode = 0;
    {
        Application application(config, std::chrono::seconds(options.durationS));
        exitCode = application.run();
    }
    LOG_INFO(app, life, "exit code={}", exitCode);
    Logger::shutdown();
    return exitCode;
}
