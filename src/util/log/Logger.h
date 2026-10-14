#pragma once

#include <string>
#include <utility>

#include <spdlog/spdlog.h>

enum class log_level {
    debug,
    info,
    warn,
    error
};

// device tag: which part of the system wrote the line
enum class log_tag {
    app,
    cfg,
    bus,
    loop,
    ctrl,
    video,
    cap,
    sync,
    enc,
    store,
    sensor,
    http,
    mqtt,
    server,
    ffmpeg,
    test
};

enum class log_category {
    life,
    cfg,
    net,
    tx,
    rx,
    cmd,
    stat,
    hw,
    prot,
    sys,
    test
};

struct log_options {
    std::string directory;
    std::string level = "info";
    bool console = true;
    size_t maxFiles = 72;
};

/**
 * @brief spdlog front end. format: [L][HH:MM:SS.mmm][thread] [tag  ][CAT ] message key=value
 *
 * initialize() installs the spdlog default logger; before that, lines go to spdlog's stock console logger
 */
class Logger {
public:
    static bool initialize(const log_options& options);
    static void shutdown();
    static void set_level(const std::string& level);

    static const char* get_tag_name(log_tag tag);
    static const char* get_category_name(log_category category);

    template <typename... Args>
    static void write(log_level level, log_tag tag, log_category category, spdlog::format_string_t<Args...> format,
                      Args&&... args) {
        spdlog::logger* logger = spdlog::default_logger_raw();
        const spdlog::level::level_enum spdLevel = to_spdlog(level);
        if (logger == nullptr || !logger->should_log(spdLevel)) {
            return;
        }
        logger->log(spdLevel, "[{:<6}][{:<4}] {}", get_tag_name(tag), get_category_name(category),
                    spdlog::fmt_lib::format(format, std::forward<Args>(args)...));
    }

private:
    static spdlog::level::level_enum to_spdlog(log_level level);
};

#define LOG_DEBUG(tag, category, ...) Logger::write(log_level::debug, log_tag::tag, log_category::category, __VA_ARGS__)
#define LOG_INFO(tag, category, ...) Logger::write(log_level::info, log_tag::tag, log_category::category, __VA_ARGS__)
#define LOG_WARN(tag, category, ...) Logger::write(log_level::warn, log_tag::tag, log_category::category, __VA_ARGS__)
#define LOG_ERROR(tag, category, ...) Logger::write(log_level::error, log_tag::tag, log_category::category, __VA_ARGS__)
