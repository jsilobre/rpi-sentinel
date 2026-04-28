#pragma once

#ifdef ENABLE_OTLP

#include "../alerts/IAlertHandler.hpp"
#include "../monitoring/Config.hpp"

#include <memory>
#include <span>

namespace rpi {

// IAlertHandler that ships sensor readings as OTLP gauge metrics
// and threshold breach/recovery events as OTLP logs to a remote
// collector (Grafana Cloud OTLP gateway in production).
//
// `sensors` is captured at construction so that per-sensor warn/crit
// thresholds can be exposed as observable gauges (`sensor.threshold.warn`,
// `sensor.threshold.crit`). They render as overlay lines on dashboards
// without the dashboard having to know the sensor list. Thresholds are
// considered static for the daemon's lifetime.
//
// Construction throws if the auth header cannot be resolved or if the
// endpoint is empty. on_event() is non-blocking: the SDK enqueues to its
// background processor.
class OtlpExporter : public IAlertHandler {
public:
    OtlpExporter(const OtlpConfig& cfg, std::span<const SensorConfig> sensors);
    ~OtlpExporter() override;

    OtlpExporter(const OtlpExporter&)            = delete;
    OtlpExporter& operator=(const OtlpExporter&) = delete;
    OtlpExporter(OtlpExporter&&)                 = delete;
    OtlpExporter& operator=(OtlpExporter&&)      = delete;

    void on_event(const SensorEvent& event) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace rpi

#endif // ENABLE_OTLP
