#include "DS18B20Reader.hpp"

#include <fstream>
#include <string>

namespace rpi {

DS18B20Reader::DS18B20Reader(std::filesystem::path device_path, std::string id, std::string metric)
    : device_path_(std::move(device_path))
    , sensor_id_(id.empty() ? device_path_.parent_path().filename().string() : std::move(id))
    , metric_(std::move(metric))
{}

auto DS18B20Reader::read() -> std::expected<SensorReading, SensorError>
{
    std::ifstream file(device_path_);
    if (!file.is_open()) {
        return std::unexpected(SensorError::DeviceNotFound);
    }

    // The kernel driver exposes temperature in millidegrees Celsius
    // e.g. /sys/bus/w1/devices/28-xxxx/temperature contains "23562"
    std::string raw;
    if (!std::getline(file, raw)) {
        return std::unexpected(SensorError::ReadFailure);
    }

    try {
        int millidegrees = std::stoi(raw);
        return SensorReading{
            .sensor_id = sensor_id_,
            .metric    = metric_,
            .value     = static_cast<float>(millidegrees) / 1000.0f,
        };
    } catch (...) {
        return std::unexpected(SensorError::ParseError);
    }
}

auto DS18B20Reader::sensor_id() const -> std::string
{
    return sensor_id_;
}

} // namespace rpi
