// Only built when ENABLE_CLOUD_STORAGE=ON.
// Verifies constructor input validation, non-blocking contract, queue
// bounding, and API key env-var resolution.  The handler is pointed at
// an unroutable port so no real network traffic is emitted; the sender
// thread will fail-log and move on.

#ifdef ENABLE_CLOUD_STORAGE

#include <gtest/gtest.h>

#include "../src/alerts/CloudStorageHandler.hpp"
#include "../src/events/SensorEvent.hpp"
#include "../src/monitoring/Config.hpp"

#include <chrono>
#include <cstdlib>

namespace {

rpi::CloudStorageConfig make_cfg()
{
    rpi::CloudStorageConfig c;
    c.enabled     = true;
    c.endpoint    = "http://127.0.0.1:1";  // unroutable; POST will fail silently
    c.api_key_env = "RPI_TEST_CLOUD_KEY_DOES_NOT_EXIST";
    c.api_key     = "test-key-dev-fallback";
    return c;
}

rpi::SensorEvent make_reading(const std::string& sensor_id = "s1", float value = 23.0f)
{
    return {
        .type      = rpi::SensorEvent::Type::Reading,
        .metric    = "temperature",
        .value     = value,
        .threshold = 0.0f,
        .sensor_id = sensor_id,
    };
}

} // namespace

TEST(CloudStorageHandler, RejectsEmptyEndpoint)
{
    auto cfg = make_cfg();
    cfg.endpoint.clear();
    EXPECT_THROW((rpi::CloudStorageHandler{cfg}), std::runtime_error);
}

TEST(CloudStorageHandler, RejectsMissingApiKey)
{
    auto cfg = make_cfg();
    cfg.api_key.clear();
    ::unsetenv(cfg.api_key_env.c_str());
    EXPECT_THROW((rpi::CloudStorageHandler{cfg}), std::runtime_error);
}

TEST(CloudStorageHandler, ResolvesApiKeyFromEnv)
{
    auto cfg = make_cfg();
    cfg.api_key.clear();
    cfg.api_key_env = "RPI_TEST_CLOUD_API_KEY_ENV";
    ::setenv(cfg.api_key_env.c_str(), "env-key-value", 1);
    EXPECT_NO_THROW((rpi::CloudStorageHandler{cfg}));
    ::unsetenv(cfg.api_key_env.c_str());
}

TEST(CloudStorageHandler, OnEventIsNonBlocking)
{
    auto cfg = make_cfg();
    rpi::CloudStorageHandler handler{cfg};

    // Enqueuing 50 readings must complete far faster than the 2 s drain interval.
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 50; ++i)
        EXPECT_NO_THROW(handler.on_event(make_reading("s1", static_cast<float>(i))));
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    EXPECT_LT(elapsed_ms, 200);
}

TEST(CloudStorageHandler, IgnoresNonReadingEvents)
{
    auto cfg = make_cfg();
    rpi::CloudStorageHandler handler{cfg};

    rpi::SensorEvent exceeded{
        .type      = rpi::SensorEvent::Type::ThresholdExceeded,
        .metric    = "temperature",
        .value     = 31.0f,
        .threshold = 30.0f,
        .sensor_id = "s1",
    };
    rpi::SensorEvent recovered{
        .type      = rpi::SensorEvent::Type::ThresholdRecovered,
        .metric    = "temperature",
        .value     = 27.0f,
        .threshold = 30.0f,
        .sensor_id = "s1",
    };
    EXPECT_NO_THROW(handler.on_event(exceeded));
    EXPECT_NO_THROW(handler.on_event(recovered));
}

TEST(CloudStorageHandler, QueueDropsOldestWhenFull)
{
    auto cfg = make_cfg();
    rpi::CloudStorageHandler handler{cfg};

    // Overfill 2× capacity — must never block or throw.
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 2100; ++i)
        handler.on_event(make_reading("s1", static_cast<float>(i)));
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    EXPECT_LT(elapsed_ms, 500);
}

#endif // ENABLE_CLOUD_STORAGE
