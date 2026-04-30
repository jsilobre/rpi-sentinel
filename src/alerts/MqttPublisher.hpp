#pragma once

#ifdef ENABLE_MQTT

#include "IAlertHandler.hpp"
#include "../monitoring/Config.hpp"
#include <expected>
#include <functional>
#include <memory>
#include <string>

struct mosquitto;
struct mosquitto_message;

namespace rpi {

class HistoryStore;

class MqttPublisher final : public IAlertHandler {
public:
    using ThresholdCallback = std::function<std::expected<void, std::string>(
                                  const std::string& sensor_id, float warn, float crit)>;
    using ForcePoller       = std::function<void()>;

    explicit MqttPublisher(const MqttConfig& config);
    ~MqttPublisher();

    void connect();
    void disconnect();

    void set_threshold_callback(ThresholdCallback cb);
    void set_force_poller(ForcePoller cb);
    void set_history_store(std::shared_ptr<HistoryStore> store);
    void publish_config(const std::string& config_json);

    void on_event(const SensorEvent& event) override;

    // Exposed for tests: build a JSON response payload for a history request.
    // Returns empty string if the request is malformed.
    std::string build_history_response(const std::string& request_payload) const;

private:
    static void on_connect_cb(struct mosquitto*, void* userdata, int rc);
    static void on_message_cb(struct mosquitto*, void* userdata,
                               const struct mosquitto_message* msg);
    void handle_connect(int rc);
    void handle_message(const struct mosquitto_message* msg);
    void handle_history_request(const std::string& payload);
    void publish(const std::string& topic, const std::string& payload, bool retain);

    MqttConfig                    config_;
    mosquitto*                    mosq_ = nullptr;
    ThresholdCallback             threshold_cb_;
    ForcePoller                   force_poller_;
    std::shared_ptr<HistoryStore> history_store_;
    std::string                   status_topic_;
    std::string                   config_topic_current_;
    std::string                   config_topic_set_;
    std::string                   history_req_topic_;
    std::string                   history_resp_prefix_;
    std::string                   cmd_refresh_topic_;
};

} // namespace rpi

#endif // ENABLE_MQTT
