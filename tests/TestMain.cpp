#include "catch_amalgamated.hpp"

#include "util/log/Logger.h"
#ifdef SYNCCAP_TESTS_WITH_COLLECTOR
#include "codec/FfmpegLog.h"
#endif

int main(int argc, char* argv[]) {
    log_options options;
    options.directory.clear();
    options.level = "warn";
    Logger::initialize(options);
#ifdef SYNCCAP_TESTS_WITH_COLLECTOR
    FfmpegLog::install();
#endif
    const int result = Catch::Session().run(argc, argv);
    Logger::shutdown();
    return result;
}
