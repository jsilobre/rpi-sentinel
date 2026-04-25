#include <gtest/gtest.h>
#include "../src/web/WebState.hpp"

#include <chrono>
#include <vector>

using namespace rpi;
using clock_t_ = std::chrono::system_clock;

TEST(WebStateHydration, PrimeSetsHasReadingAndCurrentValue)
{
    WebState ws;
    auto t0 = clock_t_::now();
    std::vector<HistoryPoint> pts = {
        {10.0f, t0},
        {11.0f, t0 + std::chrono::seconds{1}},
        {12.5f, t0 + std::chrono::seconds{2}},
    };

    ws.prime_history("s1", "temperature", pts);

    auto snap = ws.snapshot();
    ASSERT_EQ(snap.sensors.size(), 1u);
    const auto& s = snap.sensors[0];
    EXPECT_EQ(s.id,            "s1");
    EXPECT_EQ(s.metric,        "temperature");
    EXPECT_TRUE(s.has_reading);
    EXPECT_FLOAT_EQ(s.current_value, 12.5f);
    ASSERT_EQ(s.history.size(), 3u);
    EXPECT_FLOAT_EQ(s.history.front().value, 10.0f);
    EXPECT_FLOAT_EQ(s.history.back().value,  12.5f);
}

TEST(WebStateHydration, PrimeRespectsMaxHistoryCap)
{
    WebState ws;
    auto t0 = clock_t_::now();

    std::vector<HistoryPoint> pts;
    pts.reserve(WebState::MAX_HISTORY + 10);
    for (size_t i = 0; i < WebState::MAX_HISTORY + 10; ++i) {
        pts.push_back({static_cast<float>(i), t0 + std::chrono::seconds{static_cast<int>(i)}});
    }
    ws.prime_history("s1", "temperature", std::move(pts));

    auto snap = ws.snapshot();
    ASSERT_EQ(snap.sensors[0].history.size(), WebState::MAX_HISTORY);
    EXPECT_FLOAT_EQ(snap.sensors[0].history.front().value, 10.0f);  // dropped first 10
    EXPECT_FLOAT_EQ(snap.sensors[0].history.back().value,
                    static_cast<float>(WebState::MAX_HISTORY + 9));
}

TEST(WebStateHydration, EmptyHistoryDoesNotMarkAsRead)
{
    WebState ws;
    ws.prime_history("s1", "temperature", {});

    auto snap = ws.snapshot();
    ASSERT_EQ(snap.sensors.size(), 1u);
    EXPECT_FALSE(snap.sensors[0].has_reading);
    EXPECT_TRUE(snap.sensors[0].history.empty());
}

TEST(WebStateHydration, PrimeBeforeLiveReadingsKeepsOrder)
{
    WebState ws;
    auto t0 = clock_t_::now();

    ws.prime_history("s1", "temperature", {{1.0f, t0}, {2.0f, t0 + std::chrono::seconds{1}}});

    SensorEvent ev{
        .type      = SensorEvent::Type::Reading,
        .metric    = "temperature",
        .value     = 3.0f,
        .threshold = 0.0f,
        .sensor_id = "s1",
        .timestamp = t0 + std::chrono::seconds{2},
    };
    ws.push_reading(ev);

    auto snap = ws.snapshot();
    ASSERT_EQ(snap.sensors[0].history.size(), 3u);
    EXPECT_FLOAT_EQ(snap.sensors[0].history[0].value, 1.0f);
    EXPECT_FLOAT_EQ(snap.sensors[0].history[1].value, 2.0f);
    EXPECT_FLOAT_EQ(snap.sensors[0].history[2].value, 3.0f);
    EXPECT_FLOAT_EQ(snap.sensors[0].current_value,    3.0f);
}
