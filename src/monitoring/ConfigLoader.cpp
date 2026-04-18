#include "ConfigLoader.hpp"

#include <format>
#include <fstream>
#include <nlohmann/json.hpp>

namespace rpi {

static auto parse_sensor_config(const nlohmann::json& j) -> std::expected<SensorConfig, std::string>
{
    SensorConfig sc;

    if (!j.contains("id") || !j["id"].is_string())
        return std::unexpected("Each sensor must have a string 'id'");
    sc.id = j["id"].get<std::string>();

    if (!j.contains("type") || !j["type"].is_string())
        return std::unexpected(std::format("sensor '{}': missing 'type'", sc.id));

    std::string t = j["type"].get<std::string>();
    if      (t == "ds18b20")   sc.type = SensorType::DS18B20;
    else if (t == "simulated") sc.type = SensorType::Simulated;
    else return std::unexpected(std::format("sensor '{}': unknown type '{}'", sc.id, t));

    if (j.contains("device_path"))    sc.device_path    = j["device_path"].get<std::string>();
    if (j.contains("metric"))         sc.metric         = j["metric"].get<std::string>();
    if (j.contains("threshold_warn")) sc.threshold_warn = j["threshold_warn"].get<float>();
    if (j.contains("threshold_crit")) sc.threshold_crit = j["threshold_crit"].get<float>();

    if (sc.threshold_warn >= sc.threshold_crit)
        return std::unexpected(std::format(
            "sensor '{}': threshold_warn must be < threshold_crit", sc.id));

    return sc;
}

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

        if (!j.contains("sensors") || !j["sensors"].is_array())
            return std::unexpected("Config must contain a 'sensors' array");

        for (const auto& s : j["sensors"]) {
            auto sc = parse_sensor_config(s);
            if (!sc) return std::unexpected(sc.error());
            cfg.sensors.push_back(std::move(*sc));
        }

        if (cfg.sensors.empty())
            return std::unexpected("'sensors' array must not be empty");

        if (j.contains("hysteresis"))       cfg.hysteresis    = j["hysteresis"].get<float>();
        if (j.contains("poll_interval_ms")) cfg.poll_interval = std::chrono::milliseconds{
                                                j["poll_interval_ms"].get<int>()};
        if (j.contains("web_enabled"))      cfg.web_enabled   = j["web_enabled"].get<bool>();
        if (j.contains("web_port"))         cfg.web_port      = j["web_port"].get<uint16_t>();

        if (cfg.hysteresis < 0.0f)
            return std::unexpected("hysteresis must be >= 0");
        if (cfg.poll_interval.count() <= 0)
            return std::unexpected("poll_interval_ms must be > 0");

        return cfg;

    } catch (const nlohmann::json::type_error& e) {
        return std::unexpected(std::format("JSON type error: {}", e.what()));
    }
}

} // namespace rpi
