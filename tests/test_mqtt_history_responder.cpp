#include <gtest/gtest.h>

#include "../src/alerts/MqttPublisher.hpp"
#include "../src/persistence/HistoryStore.hpp"

#include <chrono>
#include <filesystem>
#include <nlohmann/json.hpp>

using namespace rpi;
using clock_t_ = std::chrono::system_clock;

namespace {
std::filesystem::path tmp_db_path()
{
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    std::string name = info ? info->name() : "unknown";
    return std::filesystem::temp_directory_path() / ("rpi_resp_" + name + ".db");
}

MqttConfig make_config()
{
    MqttConfig cfg;
    cfg.enabled      = true;
    cfg.broker_url   = "tcp://127.0.0.1:1";  // not connected, just constructed
    cfg.topic_prefix = "rpi";
    return cfg;
}
} // namespace

class MqttHistoryResponderTest : public ::testing::Test {
protected:
    std::filesystem::path                 db_path_;
    std::shared_ptr<HistoryStore>         store_;
    std::unique_ptr<MqttPublisher>        pub_;

    void SetUp() override {
        db_path_ = tmp_db_path();
        std::filesystem::remove(db_path_);
        std::filesystem::remove(db_path_.string() + "-wal");
        std::filesystem::remove(db_path_.string() + "-shm");

        store_ = std::make_shared<HistoryStore>(db_path_, 7, 1000);
        pub_   = std::make_unique<MqttPublisher>(make_config());
        pub_->set_history_store(store_);
    }

    void TearDown() override {
        pub_.reset();
        store_.reset();
        std::filesystem::remove(db_path_);
        std::filesystem::remove(db_path_.string() + "-wal");
        std::filesystem::remove(db_path_.string() + "-shm");
    }
};

TEST_F(MqttHistoryResponderTest, BuildsResponseWithStoredPoints)
{
    auto t0 = clock_t_::now();
    for (int i = 0; i < 5; ++i) {
        store_->insert("s1", "temperature",
                       static_cast<float>(20 + i),
                       t0 + std::chrono::seconds{i});
    }

    const std::string req = R"({"request_id":"abc","sensor_id":"s1","limit":10})";
    auto body = pub_->build_history_response(req);
    ASSERT_FALSE(body.empty());

    auto j = nlohmann::json::parse(body);
    EXPECT_EQ(j["request_id"], "abc");
    EXPECT_EQ(j["sensor_id"],  "s1");
    EXPECT_EQ(j["metric"],     "temperature");
    EXPECT_EQ(j["truncated"],  false);
    ASSERT_TRUE(j["points"].is_array());
    ASSERT_EQ(j["points"].size(), 5u);
    EXPECT_FLOAT_EQ(j["points"][0]["value"].get<float>(), 20.0f);
    EXPECT_FLOAT_EQ(j["points"][4]["value"].get<float>(), 24.0f);
}

TEST_F(MqttHistoryResponderTest, RespectsLimitAndSetsTruncated)
{
    auto t0 = clock_t_::now();
    for (int i = 0; i < 10; ++i) {
        store_->insert("s1", "temperature", static_cast<float>(i),
                       t0 + std::chrono::seconds{i});
    }
    const std::string req = R"({"request_id":"id1","sensor_id":"s1","limit":3})";
    auto body = pub_->build_history_response(req);
    auto j = nlohmann::json::parse(body);

    ASSERT_EQ(j["points"].size(), 3u);
    EXPECT_EQ(j["truncated"], true);
}

TEST_F(MqttHistoryResponderTest, SinceFiltersByTimestamp)
{
    auto t0 = clock_t_::now();
    for (int i = 0; i < 10; ++i) {
        store_->insert("s1", "temperature", static_cast<float>(i),
                       t0 + std::chrono::seconds{i});
    }
    const auto cutoff = std::chrono::duration_cast<std::chrono::milliseconds>(
        (t0 + std::chrono::seconds{6}).time_since_epoch()).count();

    const auto req = nlohmann::json{
        {"request_id", "x"}, {"sensor_id", "s1"},
        {"since_ts", cutoff}, {"limit", 100}}.dump();
    auto body = pub_->build_history_response(req);
    auto j = nlohmann::json::parse(body);

    ASSERT_EQ(j["points"].size(), 4u);
    EXPECT_FLOAT_EQ(j["points"][0]["value"].get<float>(), 6.0f);
}

TEST_F(MqttHistoryResponderTest, UnknownSensorReturnsEmptyPoints)
{
    const std::string req = R"({"request_id":"q","sensor_id":"ghost","limit":10})";
    auto body = pub_->build_history_response(req);
    auto j = nlohmann::json::parse(body);
    EXPECT_EQ(j["sensor_id"], "ghost");
    EXPECT_TRUE(j["points"].is_array());
    EXPECT_TRUE(j["points"].empty());
}

TEST_F(MqttHistoryResponderTest, MalformedRequestReturnsEmptyString)
{
    EXPECT_TRUE(pub_->build_history_response("not json").empty());
    EXPECT_TRUE(pub_->build_history_response(R"({"sensor_id":"s1"})").empty());
    EXPECT_TRUE(pub_->build_history_response(R"({"request_id":"x"})").empty());
}
