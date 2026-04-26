#pragma once

#ifdef ENABLE_OTLP

#include "../alerts/IAlertHandler.hpp"
#include "../monitoring/Config.hpp"

#include <memory>

namespace rpi {

// IAlertHandler that ships sensor readings as OTLP gauge metrics
// and threshold breach/recovery events as OTLP logs to a remote
// collector (Grafana Cloud OTLP gateway in production).
//
// Construction initializes the global OpenTelemetry MeterProvider and
// LoggerProvider; destruction flushes and shuts them down. Construction
// throws if the auth header cannot be resolved (env var unset and no
// literal in config) or if the endpoint is empty.
//
// on_event() is non-blocking: the SDK enqueues to its background processor.
class OtlpExporter : public IAlertHandler {
public:
    explicit OtlpExporter(const OtlpConfig& cfg);
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
