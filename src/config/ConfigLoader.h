#pragma once

#include <string>
#include <vector>

#include "config/AppConfig.h"

struct config_issues {
    std::vector<std::string> errors;    // refuse to run
    std::vector<std::string> warnings;  // run with the stated fallback
};

/**
 * @brief reads config/collector.json into app_config and checks every range
 *
 * out-of-range values that touch a hard requirement (channel count, fps, report interval, sync tolerance)
 * are errors. others fall back to the default and the warning says which default
 */
class ConfigLoader {
public:
    static bool loadFile(const std::string& path, app_config& config, config_issues& issues);
    static bool loadText(const std::string& text, app_config& config, config_issues& issues);

    // the whole configuration in key=value form for the startup log. secrets are masked
    static std::vector<std::string> summarize(const app_config& config);
};
