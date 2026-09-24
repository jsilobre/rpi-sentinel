#include <gtest/gtest.h>
#include "../src/persistence/HistoryStore.hpp"

#include <chrono>
#include <filesystem>
#include <format>
#include <sqlite3.h>

using namespace rpi;
using clock_t_ = std::chrono::system_clock;

static std::filesystem::path tmp_db_path()
{
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    std::string name = info ? info->name() : "unknown";
    return std::filesystem::temp_directory_path() / ("rpi_history_" + name + ".db");
}

class HistoryStoreTest : public ::testing::Test {
protected:
    std::filesystem::path path_;

    void SetUp() override {
        path_ = tmp_db_path();
        std::filesystem::remove(path_);
        // Also remove WAL/shm sidecars if leftover
        std::filesystem::remove(path_.string() + "-wal");
        std::filesystem::remove(path_.string() + "-shm");
    }

    void TearDown() override {
        std::filesystem::remove(path_);
        std::filesystem::remove(path_.string() + "-wal");
        std::filesystem::remove(path_.string() + "-shm");
    }
};

TEST_F(HistoryStoreTest, InsertAndRecentReturnsAscOrder)
{
    HistoryStore store(path_, /*retention_days=*/7, /*cap=*/1000);

    auto t0 = clock_t_::now();
    for (int i = 0; i < 5; ++i) {
        store.insert("s1", "temperature",
                     static_cast<float>(20 + i),
                     t0 + std::chrono::seconds{i});
    }

    auto pts = store.recent("s1", 10);
    ASSERT_EQ(pts.size(), 5u);
    EXPECT_FLOAT_EQ(pts.front().value, 20.0f);
    EXPECT_FLOAT_EQ(pts.back().value,  24.0f);
    EXPECT_LT(pts.front().ts_ms, pts.back().ts_ms);
}

TEST_F(HistoryStoreTest, RecentLimitTakesLatest)
{
    HistoryStore store(path_, 7, 1000);
    auto t0 = clock_t_::now();
    for (int i = 0; i < 10; ++i) {
        store.insert("s1", "temperature", static_cast<float>(i),
                     t0 + std::chrono::seconds{i});
    }
    auto pts = store.recent("s1", 3);
    ASSERT_EQ(pts.size(), 3u);
    EXPECT_FLOAT_EQ(pts[0].value, 7.0f);
    EXPECT_FLOAT_EQ(pts[1].value, 8.0f);
    EXPECT_FLOAT_EQ(pts[2].value, 9.0f);
}

TEST_F(HistoryStoreTest, SinceFiltersByTimestamp)
{
    HistoryStore store(path_, 7, 1000);
    auto t0 = clock_t_::now();
    for (int i = 0; i < 10; ++i) {
        store.insert("s1", "temperature", static_cast<float>(i),
                     t0 + std::chrono::seconds{i});
    }
    const auto cutoff = std::chrono::duration_cast<std::chrono::milliseconds>(
        (t0 + std::chrono::seconds{5}).time_since_epoch()).count();

    auto pts = store.since("s1", cutoff, 100);
    ASSERT_EQ(pts.size(), 5u);
    EXPECT_FLOAT_EQ(pts.front().value, 5.0f);
    EXPECT_FLOAT_EQ(pts.back().value,  9.0f);
}

TEST_F(HistoryStoreTest, SinceDownsamplesWhenLimitExceeded)
{
    HistoryStore store(path_, 7, 5000);
    auto t0 = clock_t_::now();
    for (int i = 0; i < 1000; ++i) {
        store.insert("s1", "temperature", static_cast<float>(i),
                     t0 + std::chrono::seconds{i});
    }
    const auto cutoff = std::chrono::duration_cast<std::chrono::milliseconds>(
        t0.time_since_epoch()).count();

    auto pts = store.since("s1", cutoff, 100);
    ASSERT_EQ(pts.size(), 100u);
    // Endpoints must be preserved so the curve spans the full window.
    EXPECT_FLOAT_EQ(pts.front().value, 0.0f);
    EXPECT_FLOAT_EQ(pts.back().value,  999.0f);
    // Samples must remain monotonic in time.
    for (size_t i = 1; i < pts.size(); ++i) {
        EXPECT_GT(pts[i].ts_ms, pts[i - 1].ts_ms);
    }
}

TEST_F(HistoryStoreTest, MetricForReturnsLastMetric)
{
    HistoryStore store(path_, 7, 1000);
    auto t0 = clock_t_::now();
    store.insert("s1", "temperature", 1.0f, t0);
    store.insert("s2", "humidity",   2.0f, t0 + std::chrono::seconds{1});

    auto m1 = store.metric_for("s1");
    auto m2 = store.metric_for("s2");
    auto m3 = store.metric_for("ghost");
    ASSERT_TRUE(m1.has_value()); EXPECT_EQ(*m1, "temperature");
    ASSERT_TRUE(m2.has_value()); EXPECT_EQ(*m2, "humidity");
    EXPECT_FALSE(m3.has_value());
}

TEST_F(HistoryStoreTest, RotateRespectsPerSensorCap)
{
    HistoryStore store(path_, /*retention_days=*/7, /*cap=*/3);
    auto t0 = clock_t_::now();
    for (int i = 0; i < 10; ++i) {
        store.insert("s1", "temperature", static_cast<float>(i),
                     t0 + std::chrono::seconds{i});
    }
    store.rotate();
    auto pts = store.recent("s1", 100);
    ASSERT_EQ(pts.size(), 3u);
    EXPECT_FLOAT_EQ(pts[0].value, 7.0f);
    EXPECT_FLOAT_EQ(pts[2].value, 9.0f);
}

TEST_F(HistoryStoreTest, RotateDropsAgedRows)
{
    HistoryStore store(path_, /*retention_days=*/1, /*cap=*/1000);
    const auto now = clock_t_::now();
    store.insert("s1", "temperature", 1.0f, now - std::chrono::hours{48});  // older than 1d
    store.insert("s1", "temperature", 2.0f, now - std::chrono::hours{1});   // recent
    store.rotate();
    auto pts = store.recent("s1", 100);
    ASSERT_EQ(pts.size(), 1u);
    EXPECT_FLOAT_EQ(pts[0].value, 2.0f);
}

TEST_F(HistoryStoreTest, ReopenPreservesData)
{
    auto t0 = clock_t_::now();
    {
        HistoryStore store(path_, 7, 1000);
        store.insert("s1", "temperature", 42.0f, t0);
    }
    HistoryStore store(path_, 7, 1000);
    auto pts = store.recent("s1", 10);
    ASSERT_EQ(pts.size(), 1u);
    EXPECT_FLOAT_EQ(pts[0].value, 42.0f);
}

namespace {
StoredAlert make_alert(int64_t ts_ms, std::string sensor_id, std::string level, float value)
{
    return {
        .ts_ms     = ts_ms,
        .sensor_id = std::move(sensor_id),
        .metric    = "tvoc",
        .type      = "EXCEEDED",
        .level     = std::move(level),
        .value     = value,
        .threshold = 150.0f,
    };
}

int64_t ms(clock_t_::time_point tp)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count();
}
} // namespace

TEST_F(HistoryStoreTest, InsertAlertAndRecentAlertsNewestFirst)
{
    HistoryStore store(path_, 7, 1000);
    const int64_t t0 = ms(clock_t_::now());
    store.insert_alert(make_alert(t0,        "a", "warn", 151.0f));
    store.insert_alert(make_alert(t0 + 1000, "b", "crit", 401.0f));
    store.insert_alert(make_alert(t0 + 2000, "a", "warn", 152.0f));

    auto all = store.recent_alerts(10);
    ASSERT_EQ(all.size(), 3u);
    EXPECT_EQ(all[0].ts_ms, t0 + 2000);   // newest first, across sensors
    EXPECT_EQ(all[1].sensor_id, "b");
    EXPECT_EQ(all[1].level, "crit");
    EXPECT_EQ(all[1].type, "EXCEEDED");
    EXPECT_EQ(all[1].metric, "tvoc");
    EXPECT_FLOAT_EQ(all[1].value, 401.0f);
    EXPECT_FLOAT_EQ(all[1].threshold, 150.0f);
    EXPECT_EQ(all[2].ts_ms, t0);

    auto two = store.recent_alerts(2);
    ASSERT_EQ(two.size(), 2u);
    EXPECT_EQ(two[0].ts_ms, t0 + 2000);
    EXPECT_EQ(two[1].ts_ms, t0 + 1000);

    EXPECT_TRUE(store.recent_alerts(0).empty());
}

TEST_F(HistoryStoreTest, RotateDropsAgedAlerts)
{
    HistoryStore store(path_, /*retention_days=*/1, /*cap=*/1000);
    const auto now = clock_t_::now();
    store.insert_alert(make_alert(ms(now - std::chrono::hours{48}), "a", "warn", 1.0f));
    store.insert_alert(make_alert(ms(now - std::chrono::hours{1}),  "a", "warn", 2.0f));
    store.rotate();
    auto alerts = store.recent_alerts(10);
    ASSERT_EQ(alerts.size(), 1u);
    EXPECT_FLOAT_EQ(alerts[0].value, 2.0f);
}

TEST_F(HistoryStoreTest, ClearAllRemovesAlerts)
{
    HistoryStore store(path_, 7, 1000);
    store.insert("s1", "temperature", 1.0f, clock_t_::now());
    store.insert_alert(make_alert(ms(clock_t_::now()), "a", "crit", 1.0f));
    store.clear_all();
    EXPECT_TRUE(store.recent("s1", 10).empty());
    EXPECT_TRUE(store.recent_alerts(10).empty());
}

TEST_F(HistoryStoreTest, ClearAlertsKeepsReadings)
{
    HistoryStore store(path_, 7, 1000);
    store.insert("s1", "temperature", 1.0f, clock_t_::now());
    store.insert_alert(make_alert(ms(clock_t_::now()), "a", "crit", 1.0f));
    store.clear_alerts();
    EXPECT_EQ(store.recent("s1", 10).size(), 1u);
    EXPECT_TRUE(store.recent_alerts(10).empty());
}

TEST_F(HistoryStoreTest, ReopenPreservesAlerts)
{
    const int64_t t0 = ms(clock_t_::now());
    {
        HistoryStore store(path_, 7, 1000);
        store.insert_alert(make_alert(t0, "a", "crit", 42.0f));
    }
    HistoryStore store(path_, 7, 1000);
    auto alerts = store.recent_alerts(10);
    ASSERT_EQ(alerts.size(), 1u);
    EXPECT_EQ(alerts[0].ts_ms, t0);
    EXPECT_FLOAT_EQ(alerts[0].value, 42.0f);
}

TEST_F(HistoryStoreTest, OpensVersion1DatabaseAndAddsAlertsTable)
{
    // A database written by a pre-alerts daemon (readings only, user_version 1)
    // must upgrade in place without losing its readings.
    const auto t0 = clock_t_::now();
    {
        sqlite3* db = nullptr;
        ASSERT_EQ(sqlite3_open(path_.string().c_str(), &db), SQLITE_OK);
        const std::string sql = std::format(
            "CREATE TABLE readings(sensor_id TEXT NOT NULL, ts INTEGER NOT NULL,"
            "  value REAL NOT NULL, metric TEXT NOT NULL);"
            "INSERT INTO readings VALUES('s1', {}, 21.5, 'temperature');"
            "PRAGMA user_version = 1;", ms(t0));
        ASSERT_EQ(sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr), SQLITE_OK);
        sqlite3_close(db);
    }

    HistoryStore store(path_, 7, 1000);
    ASSERT_EQ(store.recent("s1", 10).size(), 1u);

    store.insert_alert(make_alert(ms(t0), "s1", "warn", 21.5f));
    EXPECT_EQ(store.recent_alerts(10).size(), 1u);
}
