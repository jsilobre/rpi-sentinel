#pragma once

#include "ISensorReader.hpp"
#include <filesystem>
#include <string>

namespace rpi {

class DS18B20Reader final : public ISensorReader {
public:
    explicit DS18B20Reader(std::filesystem::path device_path);

    auto read() -> std::expected<SensorReading, SensorError> override;
    auto sensor_id() const -> std::string override;

private:
    std::filesystem::path device_path_;
    std::string           sensor_id_;
};

} // namespace rpi
