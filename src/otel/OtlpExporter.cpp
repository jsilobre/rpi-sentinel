#ifdef ENABLE_OTLP

#include "OtlpExporter.hpp"

#include <opentelemetry/exporters/otlp/otlp_http_log_record_exporter_factory.h>
#include <opentelemetry/exporters/otlp/otlp_http_log_record_exporter_options.h>
#include <opentelemetry/exporters/otlp/otlp_http_metric_exporter_factory.h>
#include <opentelemetry/exporters/otlp/otlp_http_metric_exporter_options.h>
#include <opentelemetry/logs/logger.h>
#include <opentelemetry/logs/provider.h>
#include <opentelemetry/metrics/async_instruments.h>
#include <opentelemetry/metrics/observer_result.h>
#include <opentelemetry/metrics/provider.h>
#include <opentelemetry/sdk/logs/batch_log_record_processor_factory.h>
#include <opentelemetry/sdk/logs/batch_log_record_processor_options.h>
#include <opentelemetry/sdk/logs/logger_provider_factory.h>
#include <opentelemetry/sdk/metrics/export/periodic_exporting_metric_reader_factory.h>
#include <opentelemetry/sdk/metrics/meter_provider.h>
#include <opentelemetry/sdk/metrics/meter_provider_factory.h>
#include <opentelemetry/sdk/metrics/view/view_registry_factory.h>
#include <opentelemetry/sdk/resource/resource.h>

#include <chrono>
#include <cstdlib>
#include <format>
#include <map>
#include <mutex>
#include <print>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace rpi {

namespace otlp_sdk = opentelemetry::sdk;
namespace otlp_exp = opentelemetry::exporter::otlp;
namespace otlp_api = opentelemetry::metrics;
namespace otlp_log = opentelemetry::logs;

namespace {

std::string resolve_auth_header(const OtlpConfig& cfg)
{
    if (!cfg.auth_header_env.empty()) {
        if (const char* v = std::getenv(cfg.auth_header_env.c_str()); v && *v != '\0') {
            return std::string{v};
        }
    }
    return cfg.auth_header;
}

std::string detect_hostname()
{
    char buf[256] = {};
    if (::gethostname(buf, sizeof(buf) - 1) == 0) return std::string{buf};
    return "unknown";
}

const char* severity_name(otlp_log::Severity s)
{
    switch (s) {
        case otlp_log::Severity::kInfo:  return "info";
        case otlp_log::Severity::kWarn:  return "warning";
        case otlp_log::Severity::kError: return "critical";
        default: return "unknown";
    }
}

} // anonymous namespace

// Latest reading per (sensor_id, metric). Updated synchronously from
// the EventBus thread; sampled by the SDK from its export thread via
// the ObservableGauge callback. Mutex-protected for the dual-thread access.
struct ReadingTable {
    struct Key {
        std::string sensor_id;
        std::string metric;
        bool operator<(const Key& o) const noexcept {
            return sensor_id < o.sensor_id || (sensor_id == o.sensor_id && metric < o.metric);
        }
    };

    std::mutex          mu;
    std::map<Key, double> values;

    void set(std::string sensor_id, std::string metric, double v)
    {
        std::lock_guard lk{mu};
        values[Key{std::move(sensor_id), std::move(metric)}] = v;
    }

    template <class F>
    void for_each(F&& f)
    {
        std::lock_guard lk{mu};
        for (const auto& [k, v] : values) f(k, v);
    }
};

struct OtlpExporter::Impl {
    OtlpConfig                                                  cfg;
    ReadingTable                                                readings;
    opentelemetry::nostd::shared_ptr<otlp_api::Meter>           meter;
    opentelemetry::nostd::shared_ptr<otlp_api::ObservableInstrument> sensor_reading;
    opentelemetry::nostd::shared_ptr<otlp_log::Logger>          logger;

    void emit_threshold_log(const SensorEvent& ev, otlp_log::Severity severity, const char* event_type)
    {
        if (!logger) return;
        auto rec = logger->CreateLogRecord();
        rec->SetSeverity(severity);
        rec->SetBody(event_type);
        rec->SetAttribute("sensor_id",  ev.sensor_id);
        rec->SetAttribute("metric",     ev.metric);
        rec->SetAttribute("event_type", event_type);
        rec->SetAttribute("severity",   severity_name(severity));
        rec->SetAttribute("value",      static_cast<double>(ev.value));
        rec->SetAttribute("threshold",  static_cast<double>(ev.threshold));
        logger->EmitLogRecord(std::move(rec));
    }
};

namespace {

void observe_readings(otlp_api::ObserverResult result, void* state)
{
    auto* table = static_cast<ReadingTable*>(state);
    if (!table) return;

    auto observer = opentelemetry::nostd::get<
        opentelemetry::nostd::shared_ptr<otlp_api::ObserverResultT<double>>>(result);
    if (!observer) return;

    table->for_each([&observer](const ReadingTable::Key& k, double v) {
        observer->Observe(v, {{"sensor_id", k.sensor_id}, {"metric", k.metric}});
    });
}

} // anonymous namespace

OtlpExporter::OtlpExporter(const OtlpConfig& cfg)
    : impl_{std::make_unique<Impl>()}
{
    impl_->cfg = cfg;

    if (cfg.endpoint.empty())
        throw std::runtime_error{"OtlpExporter: empty endpoint"};

    const std::string auth = resolve_auth_header(cfg);
    if (auth.empty())
        throw std::runtime_error{
            std::format("OtlpExporter: auth header not found (env '{}' unset and config 'auth_header' empty)",
                        cfg.auth_header_env)};

    const auto resource = otlp_sdk::resource::Resource::Create({
        {"service.name",        std::string{"rpi-sentinel"}},
        {"service.instance.id", cfg.service_instance_id},
        {"host.name",           detect_hostname()},
    });

    {
        otlp_exp::OtlpHttpMetricExporterOptions m_opts;
        m_opts.url = cfg.endpoint + "/v1/metrics";
        m_opts.http_headers.insert({"Authorization", auth});
        auto exporter = otlp_exp::OtlpHttpMetricExporterFactory::Create(m_opts);

        otlp_sdk::metrics::PeriodicExportingMetricReaderOptions r_opts;
        r_opts.export_interval_millis = std::chrono::milliseconds{cfg.export_interval_ms};
        r_opts.export_timeout_millis  = std::chrono::milliseconds{cfg.export_interval_ms / 2};
        auto reader = otlp_sdk::metrics::PeriodicExportingMetricReaderFactory::Create(
            std::move(exporter), r_opts);

        auto provider = otlp_sdk::metrics::MeterProviderFactory::Create(
            otlp_sdk::metrics::ViewRegistryFactory::Create(), resource);
        provider->AddMetricReader(std::shared_ptr<otlp_sdk::metrics::MetricReader>{std::move(reader)});

        opentelemetry::nostd::shared_ptr<otlp_api::MeterProvider> api_provider{provider.release()};
        otlp_api::Provider::SetMeterProvider(api_provider);

        impl_->meter = api_provider->GetMeter("rpi-sentinel", "1.0");
        impl_->sensor_reading = impl_->meter->CreateDoubleObservableGauge(
            "sensor.reading", "Latest sensor reading", "");
        impl_->sensor_reading->AddCallback(&observe_readings, &impl_->readings);
    }

    {
        otlp_exp::OtlpHttpLogRecordExporterOptions l_opts;
        l_opts.url = cfg.endpoint + "/v1/logs";
        l_opts.http_headers.insert({"Authorization", auth});
        auto exporter = otlp_exp::OtlpHttpLogRecordExporterFactory::Create(l_opts);

        otlp_sdk::logs::BatchLogRecordProcessorOptions b_opts;
        b_opts.schedule_delay_millis = std::chrono::milliseconds{2000};
        auto processor = otlp_sdk::logs::BatchLogRecordProcessorFactory::Create(
            std::move(exporter), b_opts);

        auto provider = otlp_sdk::logs::LoggerProviderFactory::Create(
            std::move(processor), resource);

        opentelemetry::nostd::shared_ptr<otlp_log::LoggerProvider> api_provider{provider.release()};
        otlp_log::Provider::SetLoggerProvider(api_provider);

        impl_->logger = api_provider->GetLogger("rpi-sentinel");
    }

    std::println("[otlp] Exporter initialized: endpoint={}, instance={}",
                 cfg.endpoint, cfg.service_instance_id);
}

OtlpExporter::~OtlpExporter()
{
    if (impl_->sensor_reading) {
        impl_->sensor_reading->RemoveCallback(&observe_readings, &impl_->readings);
    }
    otlp_api::Provider::SetMeterProvider(
        opentelemetry::nostd::shared_ptr<otlp_api::MeterProvider>{});
    otlp_log::Provider::SetLoggerProvider(
        opentelemetry::nostd::shared_ptr<otlp_log::LoggerProvider>{});
}

void OtlpExporter::on_event(const SensorEvent& event)
{
    using Type = SensorEvent::Type;
    switch (event.type) {
        case Type::Reading:
            impl_->readings.set(event.sensor_id, event.metric, static_cast<double>(event.value));
            break;
        case Type::ThresholdExceeded:
            impl_->emit_threshold_log(event, otlp_log::Severity::kWarn, "threshold_exceeded");
            break;
        case Type::ThresholdRecovered:
            impl_->emit_threshold_log(event, otlp_log::Severity::kInfo, "threshold_recovered");
            break;
    }
}

} // namespace rpi

#endif // ENABLE_OTLP
