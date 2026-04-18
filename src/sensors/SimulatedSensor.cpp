#include "SimulatedSensor.hpp"

#include <chrono>
#include <cmath>
#include <numbers>

namespace rpi {

SimulatedSensor::SimulatedSensor(std::string id, Generator generator)
    : sensor_id_(std::move(id))
    , generator_(std::move(generator))
{
    if (!generator_) {
        generator_ = make_sinusoidal(40.0f, 30.0f, 60.0f);
    }
}

auto SimulatedSensor::read() -> std::expected<SensorReading, SensorError>
{
    return SensorReading{
        .temperature_celsius = generator_(),
        .sensor_id           = sensor_id_,
    };
}

auto SimulatedSensor::sensor_id() const -> std::string
{
    return sensor_id_;
}

auto SimulatedSensor::make_sinusoidal(float base, float amplitude, float period_seconds)
    -> Generator
{
    return [base, amplitude, period_seconds]() -> float {
        using namespace std::chrono;
        auto now     = steady_clock::now().time_since_epoch();
        auto seconds = duration<float>(now).count();
        return base + amplitude * std::sin(2.0f * std::numbers::pi_v<float> * seconds / period_seconds);
    };
}

} // namespace rpi
