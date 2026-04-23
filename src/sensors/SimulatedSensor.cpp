#include "SimulatedSensor.hpp"

#include <chrono>
#include <cmath>
#include <numbers>

namespace rpi {

SimulatedSensor::SimulatedSensor(std::string id, std::string metric, Generator generator)
    : sensor_id_(std::move(id))
    , metric_(std::move(metric))
    , generator_(std::move(generator))
{
    if (!generator_) {
        generator_ = make_sinusoidal(40.0f, 30.0f, 60.0f);
    }
}

auto SimulatedSensor::read() -> std::expected<SensorReading, SensorError>
{
    return SensorReading{
        .sensor_id = sensor_id_,
        .metric    = metric_,
        .value     = generator_(),
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

auto SimulatedSensor::make_motion(float period_seconds, float active_seconds) -> Generator
{
    return [period_seconds, active_seconds]() -> float {
        using namespace std::chrono;
        auto now     = steady_clock::now().time_since_epoch();
        auto seconds = duration<float>(now).count();
        return (std::fmod(seconds, period_seconds) < active_seconds) ? 1.0f : 0.0f;
    };
}

auto SimulatedSensor::make_for_metric(std::string_view metric) -> Generator
{
    if (metric == "temperature") return make_sinusoidal(22.0f, 8.0f, 120.0f);
    if (metric == "humidity")    return make_sinusoidal(55.0f, 20.0f, 180.0f);
    if (metric == "pressure")    return make_sinusoidal(1013.0f, 5.0f, 300.0f);
    if (metric == "motion")      return make_motion();
    return make_sinusoidal(40.0f, 30.0f, 60.0f);
}

} // namespace rpi
