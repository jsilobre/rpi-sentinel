#pragma once

#ifdef ENABLE_MQTT

#include "IAlertHandler.hpp"
#include "../monitoring/Config.hpp"
#include <expected>
#include <functional>
#include <string>

struct mosquitto;
struct mosquitto_message;

namespace rpi {

class MqttPublisher final : public IAlertHandler {
public:
    using ThresholdCallback = std::function<std::expected<void, std::string>(
                                  const std::string& sensor_id, float warn, float crit)>;

    explicit MqttPublisher(const MqttConfig& config);
    ~MqttPublisher();

    void connect();
    void disconnect();

    void set_threshold_callback(ThresholdCallback cb);
    void publish_config(const std::string& config_json);

    void on_event(const SensorEvent& event) override;

private:
    static void on_message_cb(struct mosquitto*, void* userdata,
                               const struct mosquitto_message* msg);
    void handle_message(const struct mosquitto_message* msg);
    void publish(const std::string& topic, const std::string& payload, bool retain);

    MqttConfig         config_;
    mosquitto*         mosq_ = nullptr;
    ThresholdCallback  threshold_cb_;
    std::string        config_topic_current_;
    std::string        config_topic_set_;
};

} // namespace rpi

#endif // ENABLE_MQTT
