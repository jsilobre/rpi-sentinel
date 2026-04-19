#pragma once

#ifdef ENABLE_MQTT

#include "IAlertHandler.hpp"
#include "../monitoring/Config.hpp"

struct mosquitto;

namespace rpi {

class MqttPublisher final : public IAlertHandler {
public:
    explicit MqttPublisher(const MqttConfig& config);
    ~MqttPublisher();

    void connect();
    void disconnect();

    void on_event(const SensorEvent& event) override;

private:
    void publish(const std::string& topic, const std::string& payload, bool retain);

    MqttConfig config_;
    mosquitto* mosq_ = nullptr;
};

} // namespace rpi

#endif // ENABLE_MQTT
