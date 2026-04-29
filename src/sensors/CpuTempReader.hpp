#pragma once

#include "ISensorReader.hpp"
#include <filesystem>
#include <string>

namespace rpi {

class CpuTempReader final : public ISensorReader {
public:
    static constexpr auto kDefaultThermalPath = "/sys/class/thermal/thermal_zone0/temp";

    explicit CpuTempReader(std::string           id           = "cpu",
                           std::filesystem::path thermal_path = kDefaultThermalPath);

    auto read() -> std::expected<SensorReading, SensorError> override;
    auto sensor_id() const -> std::string override;

private:
    std::string           sensor_id_;
    std::filesystem::path thermal_path_;
};

} // namespace rpi
