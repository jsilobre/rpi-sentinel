#include "DHT11Reader.hpp"

#include <fstream>

namespace rpi {

DHT11Reader::DHT11Reader(std::string device_path, std::string id, std::string metric)
    : device_path_(std::move(device_path))
    , sensor_id_(std::move(id))
    , metric_(std::move(metric))
{}

auto DHT11Reader::read() -> std::expected<SensorReading, SensorError>
{
    std::string file;
    if (metric_ == "temperature")
        file = device_path_ + "/in_temp_input";
    else if (metric_ == "humidity")
        file = device_path_ + "/in_humidityrelative_input";
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
        .value     = static_cast<float>(raw) / 1000.0f,
    };
}

auto DHT11Reader::sensor_id() const -> std::string
{
    return sensor_id_;
}

} // namespace rpi
