#include "CpuTempReader.hpp"

#include <fstream>
#include <string>

namespace rpi {

CpuTempReader::CpuTempReader(std::string id, std::filesystem::path thermal_path)
    : sensor_id_(std::move(id))
    , thermal_path_(std::move(thermal_path))
{}

auto CpuTempReader::read() -> std::expected<SensorReading, SensorError>
{
    std::ifstream file(thermal_path_);
    if (!file.is_open()) {
        return std::unexpected(SensorError::DeviceNotFound);
    }

    // The kernel exposes CPU temperature in millidegrees Celsius
    // e.g. /sys/class/thermal/thermal_zone0/temp contains "48750" → 48.75 °C
    std::string raw;
    if (!std::getline(file, raw)) {
        return std::unexpected(SensorError::ReadFailure);
    }

    try {
        int millidegrees = std::stoi(raw);
        return SensorReading{
            .sensor_id = sensor_id_,
            .metric    = "temperature",
            .value     = static_cast<float>(millidegrees) / 1000.0f,
        };
    } catch (...) {
        return std::unexpected(SensorError::ParseError);
    }
}

auto CpuTempReader::sensor_id() const -> std::string
{
    return sensor_id_;
}

} // namespace rpi
