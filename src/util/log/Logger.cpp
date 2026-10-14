#include "util/log/Logger.h"

#include <cstdio>
#include <ctime>

#include <filesystem>
#include <memory>
#include <system_error>
#include <vector>

#include <spdlog/sinks/hourly_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace {

constexpr const char* LOG_PATTERN = "[%L][%H:%M:%S.%e][%t] %v";
constexpr const char* LOGGER_NAME = "synccap";
constexpr unsigned char UTF8_BOM[] = {0xEF, 0xBB, 0xBF};
constexpr int TM_BASE_YEAR = 1900;

// logs/YYYY-MM-DD-HH.log as the logging convention asks
struct hour_file_name {
    static spdlog::filename_t calc_filename(const spdlog::filename_t& directory, const tm& now) {
        return spdlog::fmt_lib::format("{}/{:04d}-{:02d}-{:02d}-{:02d}.log", directory, now.tm_year + TM_BASE_YEAR,
                                       now.tm_mon + 1, now.tm_mday, now.tm_hour);
    }
};

using hour_file_sink_t = spdlog::sinks::hourly_file_sink<std::mutex, hour_file_name>;

}  // namespace

bool Logger::initialize(const log_options& options) {
    std::vector<spdlog::sink_ptr> sinks;
    if (options.console) {
        sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
    }
    if (!options.directory.empty()) {
        std::error_code error;
        std::filesystem::create_directories(options.directory, error);
        if (error) {
            std::fprintf(stderr, "log directory create failed path=%s message=%s\n", options.directory.c_str(),
                         error.message().c_str());
            return false;
        }
        spdlog::file_event_handlers handlers;
        // a BOM lets Notepad open the Korean text correctly
        handlers.after_open = [](const spdlog::filename_t&, std::FILE* file) {
            if (file != nullptr && std::ftell(file) == 0) {
                std::fwrite(UTF8_BOM, 1, sizeof(UTF8_BOM), file);
            }
        };
        try {
            sinks.push_back(std::make_shared<hour_file_sink_t>(options.directory, false,
                                                               static_cast<uint16_t>(options.maxFiles), handlers));
        } catch (const spdlog::spdlog_ex& e) {
            std::fprintf(stderr, "log file open failed message=%s\n", e.what());
            return false;
        }
    }
    auto logger = std::make_shared<spdlog::logger>(LOGGER_NAME, sinks.begin(), sinks.end());
    logger->set_pattern(LOG_PATTERN);
    logger->flush_on(spdlog::level::warn);
    spdlog::set_default_logger(logger);
    spdlog::flush_every(std::chrono::seconds(1));
    set_level(options.level);
    return true;
}

void Logger::shutdown() {
    spdlog::shutdown();
}

void Logger::set_level(const std::string& level) {
    spdlog::set_level(spdlog::level::from_str(level));
}

const char* Logger::get_tag_name(log_tag tag) {
    switch (tag) {
        case log_tag::app:
            return "app";
        case log_tag::cfg:
            return "cfg";
        case log_tag::bus:
            return "bus";
        case log_tag::loop:
            return "loop";
        case log_tag::ctrl:
            return "ctrl";
        case log_tag::video:
            return "video";
        case log_tag::cap:
            return "cap";
        case log_tag::sync:
            return "sync";
        case log_tag::enc:
            return "enc";
        case log_tag::store:
            return "store";
        case log_tag::sensor:
            return "sensor";
        case log_tag::http:
            return "http";
        case log_tag::mqtt:
            return "mqtt";
        case log_tag::server:
            return "server";
        case log_tag::ffmpeg:
            return "ffmpeg";
        case log_tag::test:
            return "test";
        default:
            return "?";
    }
}

const char* Logger::get_category_name(log_category category) {
    switch (category) {
        case log_category::life:
            return "LIFE";
        case log_category::cfg:
            return "CFG";
        case log_category::net:
            return "NET";
        case log_category::tx:
            return "TX";
        case log_category::rx:
            return "RX";
        case log_category::cmd:
            return "CMD";
        case log_category::stat:
            return "STAT";
        case log_category::hw:
            return "HW";
        case log_category::prot:
            return "PROT";
        case log_category::sys:
            return "SYS";
        case log_category::test:
            return "TEST";
        default:
            return "?";
    }
}

spdlog::level::level_enum Logger::to_spdlog(log_level level) {
    switch (level) {
        case log_level::debug:
            return spdlog::level::debug;
        case log_level::info:
            return spdlog::level::info;
        case log_level::warn:
            return spdlog::level::warn;
        case log_level::error:
            return spdlog::level::err;
        default:
            return spdlog::level::info;
    }
}
