#include "SGP30Reader.hpp"

#include <fstream>

namespace rpi {

SGP30Reader::SGP30Reader(std::string device_path, std::string id, std::string metric)
    : device_path_(std::move(device_path))
    , sensor_id_(std::move(id))
    , metric_(std::move(metric))
{}

auto SGP30Reader::read() -> std::expected<SensorReading, SensorError>
{
    std::string file;
    if (metric_ == "eco2")
        file = device_path_ + "/in_concentration_co2_input";
    else if (metric_ == "tvoc")
        file = device_path_ + "/in_concentration_voc_input";
    else
        return std::unexpected(SensorError::ParseError);

    std::ifstream f(file);
    if (!f.is_open())
        return std::unexpected(SensorError::DeviceNotFound);

    int raw;
    if (!(f >> raw))
        return std::unexpected(SensorError::ReadFailure);

    return SensorReading{
        .sensor_id = sensor_id_,
        .metric    = metric_,
        .value     = static_cast<float>(raw),
    };
}

auto SGP30Reader::sensor_id() const -> std::string
{
    return sensor_id_;
}

} // namespace rpi
