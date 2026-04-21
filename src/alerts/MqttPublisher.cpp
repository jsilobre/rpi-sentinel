#ifdef ENABLE_MQTT

#include "MqttPublisher.hpp"

#include <mosquitto.h>
#include <format>
#include <print>
#include <stdexcept>

namespace rpi {

// ─── Helpers ──────────────────────────────────────────────────────────────────

struct BrokerAddress {
    std::string host;
    int         port;
    bool        tls;
};

static BrokerAddress parse_broker_url(const std::string& url)
{
    std::string rest    = url;
    bool        use_tls = false;
    int         port    = 1883;

    if (rest.starts_with("ssl://") || rest.starts_with("mqtts://")) {
        use_tls = true;
        port    = 8883;
        rest    = rest.substr(rest.find("://") + 3);
    } else if (rest.starts_with("tcp://") || rest.starts_with("mqtt://")) {
        rest = rest.substr(rest.find("://") + 3);
    }

    auto colon = rest.rfind(':');
    if (colon != std::string::npos) {
        return {rest.substr(0, colon), std::stoi(rest.substr(colon + 1)), use_tls};
    }
    return {rest, port, use_tls};
}

static std::string format_iso8601(std::chrono::system_clock::time_point tp)
{
    auto tt = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
    gmtime_r(&tt, &tm);
    return std::format("{:04d}-{:02d}-{:02d}T{:02d}:{:02d}:{:02d}Z",
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
        tm.tm_hour, tm.tm_min, tm.tm_sec);
}

// ─── MqttPublisher ────────────────────────────────────────────────────────────

MqttPublisher::MqttPublisher(const MqttConfig& config)
    : config_(config)
{
    mosquitto_lib_init();
    mosq_ = mosquitto_new(nullptr, /*clean_session=*/true, nullptr);
    if (!mosq_) throw std::runtime_error("Failed to create mosquitto instance");

    if (!config_.username.empty()) {
        mosquitto_username_pw_set(mosq_, config_.username.c_str(), config_.password.c_str());
    }
}

MqttPublisher::~MqttPublisher()
{
    if (mosq_) {
        mosquitto_destroy(mosq_);
        mosq_ = nullptr;
    }
    mosquitto_lib_cleanup();
}

void MqttPublisher::connect()
{
    auto addr = parse_broker_url(config_.broker_url);

    if (addr.tls) {
        // Use system CA certificates directory for TLS verification
        int rc = mosquitto_tls_set(mosq_,
            /*cafile=*/nullptr, /*capath=*/"/etc/ssl/certs",
            /*certfile=*/nullptr, /*keyfile=*/nullptr, /*pw_callback=*/nullptr);
        if (rc != MOSQ_ERR_SUCCESS) {
            throw std::runtime_error(std::format(
                "mosquitto_tls_set failed: {}", mosquitto_strerror(rc)));
        }
    }

    const std::string status_topic   = config_.topic_prefix + "/status";
    const std::string offline_payload = R"({"status":"offline"})";
    mosquitto_will_set(mosq_, status_topic.c_str(),
        static_cast<int>(offline_payload.size()), offline_payload.c_str(),
        /*qos=*/1, /*retain=*/true);

    int rc = mosquitto_connect(mosq_, addr.host.c_str(), addr.port, /*keepalive=*/60);
    if (rc != MOSQ_ERR_SUCCESS) {
        throw std::runtime_error(std::format(
            "MQTT connect to {}:{} failed: {}", addr.host, addr.port, mosquitto_strerror(rc)));
    }

    mosquitto_loop_start(mosq_);
    publish(status_topic, R"({"status":"online"})", true);
    std::println("[MqttPublisher] Connected to {}:{}", addr.host, addr.port);
}

void MqttPublisher::disconnect()
{
    if (mosq_) {
        publish(config_.topic_prefix + "/status", R"({"status":"offline"})", true);
        mosquitto_loop_stop(mosq_, false);
        mosquitto_disconnect(mosq_);
        std::println("[MqttPublisher] Disconnected");
    }
}

void MqttPublisher::on_event(const SensorEvent& event)
{
    std::string topic;
    std::string payload;
    bool        retain = false;

    const std::string ts = format_iso8601(event.timestamp);

    if (event.type == SensorEvent::Type::Reading) {
        topic  = std::format("{}/{}/reading", config_.topic_prefix, event.sensor_id);
        payload = std::format(
            "{{\"value\":{:.2f},\"metric\":\"{}\",\"timestamp\":\"{}\"}}",
            event.value, event.metric, ts);
        retain = true;
    } else {
        std::string_view type_str =
            (event.type == SensorEvent::Type::ThresholdExceeded) ? "EXCEEDED" : "RECOVERED";
        topic  = std::format("{}/{}/alert", config_.topic_prefix, event.sensor_id);
        payload = std::format(
            "{{\"type\":\"{}\",\"value\":{:.2f},\"threshold\":{:.2f},"
            "\"metric\":\"{}\",\"timestamp\":\"{}\"}}",
            type_str, event.value, event.threshold, event.metric, ts);
    }

    publish(topic, payload, retain);
}

void MqttPublisher::publish(const std::string& topic, const std::string& payload, bool retain)
{
    int rc = mosquitto_publish(mosq_,
        /*mid=*/nullptr, topic.c_str(),
        static_cast<int>(payload.size()), payload.c_str(),
        /*qos=*/1, retain);
    if (rc != MOSQ_ERR_SUCCESS) {
        std::println(stderr, "[MqttPublisher] Publish to '{}' failed: {}",
            topic, mosquitto_strerror(rc));
    }
}

} // namespace rpi

#endif // ENABLE_MQTT
