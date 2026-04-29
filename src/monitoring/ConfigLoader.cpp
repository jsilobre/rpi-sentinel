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
    else if (t == "dht11")     sc.type = SensorType::DHT11;
    else if (t == "cpu_temp")  sc.type = SensorType::CpuTemp;
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

        if (j.contains("mqtt") && j["mqtt"].is_object()) {
            const auto& m = j["mqtt"];
            if (m.contains("enabled"))      cfg.mqtt.enabled      = m["enabled"].get<bool>();
            if (m.contains("broker_url"))   cfg.mqtt.broker_url   = m["broker_url"].get<std::string>();
            if (m.contains("username"))     cfg.mqtt.username     = m["username"].get<std::string>();
            if (m.contains("password"))     cfg.mqtt.password     = m["password"].get<std::string>();
            if (m.contains("topic_prefix")) cfg.mqtt.topic_prefix = m["topic_prefix"].get<std::string>();
        }

        if (j.contains("history") && j["history"].is_object()) {
            const auto& h = j["history"];
            if (h.contains("enabled"))               cfg.history.enabled               = h["enabled"].get<bool>();
            if (h.contains("db_path"))               cfg.history.db_path               = h["db_path"].get<std::string>();
            if (h.contains("retention_days"))        cfg.history.retention_days        = h["retention_days"].get<int>();
            if (h.contains("max_points_per_sensor")) cfg.history.max_points_per_sensor = h["max_points_per_sensor"].get<int>();
        }

        if (j.contains("otlp") && j["otlp"].is_object()) {
            const auto& o = j["otlp"];
            if (o.contains("enabled"))             cfg.otlp.enabled             = o["enabled"].get<bool>();
            if (o.contains("endpoint"))            cfg.otlp.endpoint            = o["endpoint"].get<std::string>();
            if (o.contains("auth_header"))         cfg.otlp.auth_header         = o["auth_header"].get<std::string>();
            if (o.contains("auth_header_env"))     cfg.otlp.auth_header_env     = o["auth_header_env"].get<std::string>();
            if (o.contains("service_instance_id")) cfg.otlp.service_instance_id = o["service_instance_id"].get<std::string>();
            if (o.contains("export_interval_ms"))  cfg.otlp.export_interval_ms  = o["export_interval_ms"].get<int>();
        }

        if (cfg.hysteresis < 0.0f)
            return std::unexpected("hysteresis must be >= 0");
        if (cfg.poll_interval.count() <= 0)
            return std::unexpected("poll_interval_ms must be > 0");
        if (cfg.history.retention_days < 1)
            return std::unexpected("history.retention_days must be >= 1");
        if (cfg.history.max_points_per_sensor < 1)
            return std::unexpected("history.max_points_per_sensor must be >= 1");
        if (cfg.otlp.enabled && cfg.otlp.endpoint.empty())
            return std::unexpected("otlp.enabled=true but otlp.endpoint is empty");
        if (cfg.otlp.export_interval_ms <= 0)
            return std::unexpected("otlp.export_interval_ms must be > 0");

        return cfg;

    } catch (const nlohmann::json::type_error& e) {
        return std::unexpected(std::format("JSON type error: {}", e.what()));
    }
}

auto save_config(const std::filesystem::path& path, const Config& config) -> std::expected<void, std::string>
{
    auto sensor_type_str = [](SensorType t) -> std::string {
        switch (t) {
            case SensorType::DS18B20:   return "ds18b20";
            case SensorType::DHT11:     return "dht11";
            case SensorType::Simulated: return "simulated";
            case SensorType::CpuTemp:   return "cpu_temp";
        }
        return "simulated";
    };

    nlohmann::json j;
    j["hysteresis"]       = config.hysteresis;
    j["poll_interval_ms"] = static_cast<int>(config.poll_interval.count());
    j["web_enabled"]      = config.web_enabled;
    j["web_port"]         = config.web_port;
    j["mqtt"] = {
        {"enabled",      config.mqtt.enabled},
        {"broker_url",   config.mqtt.broker_url},
        {"username",     config.mqtt.username},
        {"password",     config.mqtt.password},
        {"topic_prefix", config.mqtt.topic_prefix},
    };
    j["history"] = {
        {"enabled",               config.history.enabled},
        {"db_path",               config.history.db_path},
        {"retention_days",        config.history.retention_days},
        {"max_points_per_sensor", config.history.max_points_per_sensor},
    };
    j["otlp"] = {
        {"enabled",             config.otlp.enabled},
        {"endpoint",            config.otlp.endpoint},
        {"auth_header_env",     config.otlp.auth_header_env},
        {"service_instance_id", config.otlp.service_instance_id},
        {"export_interval_ms",  config.otlp.export_interval_ms},
    };
    if (!config.otlp.auth_header.empty())
        j["otlp"]["auth_header"] = config.otlp.auth_header;

    nlohmann::json sensors = nlohmann::json::array();
    for (const auto& sc : config.sensors) {
        nlohmann::json s;
        s["id"]             = sc.id;
        s["type"]           = sensor_type_str(sc.type);
        s["metric"]         = sc.metric;
        s["threshold_warn"] = sc.threshold_warn;
        s["threshold_crit"] = sc.threshold_crit;
        if (!sc.device_path.empty())
            s["device_path"] = sc.device_path;
        sensors.push_back(std::move(s));
    }
    j["sensors"] = std::move(sensors);

    const auto tmp_path = std::filesystem::path{path.string() + ".tmp"};
    try {
        std::ofstream out(tmp_path);
        if (!out.is_open())
            return std::unexpected(std::format("Cannot write to: {}", tmp_path.string()));
        out << j.dump(4) << '\n';
        out.close();
        std::filesystem::rename(tmp_path, path);
    } catch (const std::exception& e) {
        return std::unexpected(std::format("save_config failed: {}", e.what()));
    }
    return {};
}

} // namespace rpi
