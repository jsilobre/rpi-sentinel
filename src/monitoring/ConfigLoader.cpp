#include "ConfigLoader.hpp"

#include <format>
#include <fstream>
#include <nlohmann/json.hpp>

namespace rpi {

auto load_config(const std::filesystem::path& path) -> std::expected<Config, std::string>
{
    std::ifstream file(path);
    if (!file.is_open()) {
        return std::unexpected(std::format("Cannot open config file: {}", path.string()));
    }

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(file, /*cb=*/nullptr, /*exceptions=*/true,
                                  /*ignore_comments=*/true);
    } catch (const nlohmann::json::parse_error& e) {
        return std::unexpected(std::format("JSON parse error: {}", e.what()));
    }

    try {
        Config cfg;

        if (j.contains("sensor_type")) {
            std::string t = j["sensor_type"].get<std::string>();
            if (t == "ds18b20") {
                cfg.sensor_type = SensorType::DS18B20;
            } else if (t == "simulated") {
                cfg.sensor_type = SensorType::Simulated;
            } else {
                return std::unexpected(std::format("Unknown sensor_type: '{}'", t));
            }
        }

        if (j.contains("device_path"))      cfg.device_path     = j["device_path"].get<std::string>();
        if (j.contains("threshold_warn"))   cfg.threshold_warn  = j["threshold_warn"].get<float>();
        if (j.contains("threshold_crit"))   cfg.threshold_crit  = j["threshold_crit"].get<float>();
        if (j.contains("hysteresis"))       cfg.hysteresis      = j["hysteresis"].get<float>();
        if (j.contains("poll_interval_ms")) cfg.poll_interval   = std::chrono::milliseconds{
                                                j["poll_interval_ms"].get<int>()};

        if (cfg.threshold_warn >= cfg.threshold_crit) {
            return std::unexpected("threshold_warn must be strictly less than threshold_crit");
        }
        if (cfg.hysteresis < 0.0f) {
            return std::unexpected("hysteresis must be >= 0");
        }
        if (cfg.poll_interval.count() <= 0) {
            return std::unexpected("poll_interval_ms must be > 0");
        }

        return cfg;

    } catch (const nlohmann::json::type_error& e) {
        return std::unexpected(std::format("JSON type error: {}", e.what()));
    }
}

} // namespace rpi
