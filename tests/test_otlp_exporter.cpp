// Only built when ENABLE_OTLP=ON. Verifies constructor input validation
// and that on_event() is non-throwing for each event type. The exporter
// is pointed at a non-routable host so no real network traffic is emitted;
// the SDK queues records to its background processor and the destructor
// drains them on shutdown timeout.

#include <gtest/gtest.h>

#include "../src/otel/OtlpExporter.hpp"
#include "../src/events/SensorEvent.hpp"
#include "../src/monitoring/Config.hpp"

#include <cstdlib>
#include <span>
#include <vector>

namespace {

rpi::OtlpConfig make_cfg()
{
    rpi::OtlpConfig c;
    c.enabled             = true;
    c.endpoint            = "http://127.0.0.1:1";  // unroutable port; never fires
    c.auth_header         = "Basic ZHVtbXk6ZHVtbXk=";
    c.auth_header_env     = "RPI_TEST_OTLP_TOKEN_DOES_NOT_EXIST";
    c.service_instance_id = "test-instance";
    c.export_interval_ms  = 60000;                  // long interval — no flush during test
    return c;
}

} // namespace

TEST(OtlpExporter, RejectsEmptyEndpoint)
{
    auto cfg = make_cfg();
    cfg.endpoint.clear();
    EXPECT_THROW((rpi::OtlpExporter{cfg, std::span<const rpi::SensorConfig>{}}), std::runtime_error);
}

TEST(OtlpExporter, RejectsMissingAuth)
{
    auto cfg = make_cfg();
    cfg.auth_header.clear();
    // Make sure the env fallback fails too.
    ::unsetenv(cfg.auth_header_env.c_str());
    EXPECT_THROW((rpi::OtlpExporter{cfg, std::span<const rpi::SensorConfig>{}}), std::runtime_error);
}

TEST(OtlpExporter, ConstructsAndAcceptsAllEventTypes)
{
    auto cfg = make_cfg();
    std::vector<rpi::SensorConfig> sensors;
    rpi::SensorConfig s;
    s.id             = "temp1";
    s.metric         = "temperature";
    s.threshold_warn = 30.0f;
    s.threshold_crit = 40.0f;
    sensors.push_back(std::move(s));
    rpi::OtlpExporter exporter{cfg, sensors};

    rpi::SensorEvent reading{
        .type      = rpi::SensorEvent::Type::Reading,
        .metric    = "temperature",
        .value     = 23.4f,
        .threshold = 0.0f,
        .sensor_id = "temp1",
    };
    rpi::SensorEvent exceeded{
        .type      = rpi::SensorEvent::Type::ThresholdExceeded,
        .metric    = "temperature",
        .value     = 31.2f,
        .threshold = 30.0f,
        .sensor_id = "temp1",
    };
    rpi::SensorEvent recovered{
        .type      = rpi::SensorEvent::Type::ThresholdRecovered,
        .metric    = "temperature",
        .value     = 27.8f,
        .threshold = 30.0f,
        .sensor_id = "temp1",
    };

    EXPECT_NO_THROW(exporter.on_event(reading));
    EXPECT_NO_THROW(exporter.on_event(exceeded));
    EXPECT_NO_THROW(exporter.on_event(recovered));
}

TEST(OtlpExporter, ResolvesAuthHeaderFromEnv)
{
    auto cfg = make_cfg();
    cfg.auth_header.clear();
    cfg.auth_header_env = "RPI_TEST_OTLP_AUTH_FROM_ENV";
    ::setenv(cfg.auth_header_env.c_str(), "Basic ZW52OnZhbHVl", 1);

    EXPECT_NO_THROW((rpi::OtlpExporter{cfg, std::span<const rpi::SensorConfig>{}}));

    ::unsetenv(cfg.auth_header_env.c_str());
}
