#pragma once

#ifdef ENABLE_MQTT

#include "IAlertHandler.hpp"
#include "../monitoring/Config.hpp"
#include "../persistence/HistoryStore.hpp"  // StoredAlert (std::deque needs a complete type)
#include <chrono>
#include <condition_variable>
#include <deque>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

struct mosquitto;
struct mosquitto_message;

namespace rpi {


class MqttPublisher final : public IAlertHandler {
public:
    using ThresholdCallback = std::function<std::expected<void, std::string>(
                                  const std::string& sensor_id, float warn, float crit)>;
    using PollIntervalCallback = std::function<std::expected<void, std::string>(
                                  std::chrono::milliseconds interval)>;
    using ForcePoller       = std::function<void()>;
    using DataClearer       = std::function<void()>;

    explicit MqttPublisher(const MqttConfig& config);
    ~MqttPublisher();

    void connect();
    void disconnect();

    void set_threshold_callback(ThresholdCallback cb);
    void set_poll_interval_callback(PollIntervalCallback cb);
    void set_force_poller(ForcePoller cb);
    void set_data_clearer(DataClearer cb);
    void set_history_store(std::shared_ptr<HistoryStore> store);
    void publish_config(const std::string& config_json);

    void on_event(const SensorEvent& event) override;

    // Exposed for tests: build a JSON response payload for a history request.
    // Returns empty string if the request is malformed.
    std::string build_history_response(const std::string& request_payload) const;

    // Exposed for tests: JSON body of the retained <prefix>/alerts/recent
    // snapshot, `alerts` newest first.
    static std::string build_alerts_snapshot(const std::deque<StoredAlert>& alerts);

    // Exposed for tests: the interval carried by a {"poll_interval_ms": N}
    // config/set payload, or an error if it is missing, not an integer or
    // outside [MIN_POLL_INTERVAL, MAX_POLL_INTERVAL].
    static std::expected<std::chrono::milliseconds, std::string>
        parse_poll_interval(const std::string& payload);

    // Size of that snapshot; matches the dashboard's MAX_EVENTS.
    static constexpr std::size_t RECENT_ALERTS_MAX = 50;

private:
    static void on_connect_cb(struct mosquitto*, void* userdata, int rc);
    static void on_message_cb(struct mosquitto*, void* userdata,
                               const struct mosquitto_message* msg);
    void handle_connect(int rc);
    void handle_message(const struct mosquitto_message* msg);
    void handle_history_request(const std::string& payload);
    void publish(const std::string& topic, const std::string& payload, bool retain);
    void enqueue_publish(std::string topic, std::string payload, bool retain);
    void run_publisher(std::stop_token stop);
    std::string alerts_snapshot();  // locks alerts_mu_

    struct PublishItem {
        std::string topic;
        std::string payload;
        bool        retain;
    };

    MqttConfig                    config_;
    mosquitto*                    mosq_ = nullptr;
    ThresholdCallback             threshold_cb_;
    PollIntervalCallback          poll_interval_cb_;
    ForcePoller                   force_poller_;
    DataClearer                   data_clearer_;
    std::shared_ptr<HistoryStore> history_store_;
    std::string                   status_topic_;
    std::string                   config_topic_current_;
    std::string                   config_topic_set_;
    std::string                   history_req_topic_;
    std::string                   history_resp_prefix_;
    std::string                   cmd_refresh_topic_;
    std::string                   cmd_clear_topic_;
    std::string                   cmd_clear_alerts_topic_;
    std::string                   alerts_topic_;

    // Most recent alerts, newest first. Seeded from the history store at
    // connect() and republished retained on every change, so a dashboard
    // opened later still sees the alert timeline.
    std::deque<StoredAlert>       recent_alerts_;
    std::mutex                    alerts_mu_;

    std::queue<PublishItem>       pub_queue_;
    std::mutex                    pub_mu_;
    std::condition_variable_any   pub_cv_;
    std::jthread                  pub_worker_;
};

} // namespace rpi

#endif // ENABLE_MQTT
