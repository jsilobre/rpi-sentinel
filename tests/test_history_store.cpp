#include <gtest/gtest.h>
#include "../src/persistence/HistoryStore.hpp"

#include <chrono>
#include <filesystem>

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
