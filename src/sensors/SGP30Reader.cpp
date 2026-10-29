#include "SGP30Reader.hpp"

#include <fstream>

namespace rpi {

SGP30Reader::SGP30Reader(std::string device_path, std::string id, std::string metric,
                         Sgp30Options options)
    : device_path_(std::move(device_path))
    , sensor_id_(std::move(id))
    , metric_(std::move(metric))
{
    if (device_path_.starts_with("/dev/"))
        device_ = Sgp30Device::acquire(device_path_, options);
}

SGP30Reader::SGP30Reader(std::shared_ptr<Sgp30Device> device, std::string id, std::string metric)
    : sensor_id_(std::move(id))
    , metric_(std::move(metric))
    , device_(std::move(device))
{}

auto SGP30Reader::read() -> std::expected<SensorReading, SensorError>
{
    if (metric_ != "eco2" && metric_ != "tvoc")
        return std::unexpected(SensorError::ParseError);
    return device_ ? read_i2c() : read_iio();
}

auto SGP30Reader::read_i2c() -> std::expected<SensorReading, SensorError>
{
    // Empty while the chip warms up (first 15 s) or stops answering.
    auto sample = device_->latest();
    if (!sample)
        return std::unexpected(SensorError::ReadFailure);

    return SensorReading{
        .sensor_id = sensor_id_,
        .metric    = metric_,
        .value     = static_cast<float>(metric_ == "eco2" ? sample->eco2_ppm : sample->tvoc_ppb),
    };
}

auto SGP30Reader::read_iio() -> std::expected<SensorReading, SensorError>
{
    const std::string file = device_path_ + (metric_ == "eco2"
        ? "/in_concentration_co2_input"
        : "/in_concentration_voc_input");

    std::ifstream f(file);
    if (!f.is_open())
        return std::unexpected(SensorError::DeviceNotFound);

    // IIO concentration values are dimensionless fractions: multiply to get ppm/ppb.
    double raw{};
    if (!(f >> raw))
        return std::unexpected(SensorError::ReadFailure);

    double scale = (metric_ == "eco2") ? 1e6 : 1e9;
    return SensorReading{
        .sensor_id = sensor_id_,
        .metric    = metric_,
        .value     = static_cast<float>(raw * scale),
    };
}

auto SGP30Reader::sensor_id() const -> std::string
{
    return sensor_id_;
}

} // namespace rpi
