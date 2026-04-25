#ifdef ENABLE_MQTT

#include "MqttPublisher.hpp"
#include "../persistence/HistoryStore.hpp"

#include <mosquitto.h>
#include <algorithm>
#include <chrono>
#include <format>
#include <nlohmann/json.hpp>
#include <print>
#include <stdexcept>
#include <thread>

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
    mosq_ = mosquitto_new(nullptr, /*clean_session=*/true, /*userdata=*/this);
    if (!mosq_) throw std::runtime_error("Failed to create mosquitto instance");

    if (!config_.username.empty()) {
        mosquitto_username_pw_set(mosq_, config_.username.c_str(), config_.password.c_str());
    }

    mosquitto_connect_callback_set(mosq_, &MqttPublisher::on_connect_cb);
    mosquitto_message_callback_set(mosq_, &MqttPublisher::on_message_cb);
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

    mosquitto_reconnect_delay_set(mosq_, /*delay=*/1, /*delay_max=*/30, /*exponential_backoff=*/true);

    int rc = mosquitto_connect(mosq_, addr.host.c_str(), addr.port, /*keepalive=*/30);
    if (rc != MOSQ_ERR_SUCCESS) {
        throw std::runtime_error(std::format(
            "MQTT connect to {}:{} failed: {}", addr.host, addr.port, mosquitto_strerror(rc)));
    }

    status_topic_         = config_.topic_prefix + "/status";
    config_topic_current_ = config_.topic_prefix + "/config/current";
    config_topic_set_     = config_.topic_prefix + "/config/set";
    history_req_topic_    = config_.topic_prefix + "/history/req";
    history_resp_prefix_  = config_.topic_prefix + "/history/resp/";

    mosquitto_loop_start(mosq_);
    std::println("[MqttPublisher] Connecting to {}:{}…", addr.host, addr.port);
}

void MqttPublisher::disconnect()
{
    if (mosq_) {
        const std::string topic   = config_.topic_prefix + "/status";
        const std::string payload = R"({"status":"offline"})";
        // QoS 1 + retain — let the loop flush the PUBACK before we disconnect
        mosquitto_publish(mosq_, nullptr, topic.c_str(),
            static_cast<int>(payload.size()), payload.c_str(), /*qos=*/1, /*retain=*/true);
        std::this_thread::sleep_for(std::chrono::milliseconds{500});
        mosquitto_disconnect(mosq_);                 // send MQTT DISCONNECT (loop thread will exit)
        mosquitto_loop_stop(mosq_, /*force=*/false); // join loop thread cleanly
        std::println("[MqttPublisher] Disconnected");
    }
}

void MqttPublisher::on_connect_cb(struct mosquitto*, void* userdata, int rc)
{
    if (userdata) static_cast<MqttPublisher*>(userdata)->handle_connect(rc);
}

void MqttPublisher::handle_connect(int rc)
{
    if (rc != 0) {
        std::println(stderr, "[MqttPublisher] CONNACK error: {}", rc);
        return;
    }
    mosquitto_subscribe(mosq_, nullptr, config_topic_set_.c_str(), /*qos=*/1);
    mosquitto_subscribe(mosq_, nullptr, history_req_topic_.c_str(), /*qos=*/1);
    publish(status_topic_, R"({"status":"online"})", /*retain=*/true);
    std::println("[MqttPublisher] Connected and online");
}

void MqttPublisher::set_threshold_callback(ThresholdCallback cb)
{
    threshold_cb_ = std::move(cb);
}

void MqttPublisher::set_history_store(std::shared_ptr<HistoryStore> store)
{
    history_store_ = std::move(store);
}

void MqttPublisher::publish_config(const std::string& config_json)
{
    if (!config_topic_current_.empty())
        publish(config_topic_current_, config_json, /*retain=*/true);
}

void MqttPublisher::on_message_cb(struct mosquitto*, void* userdata,
                                   const struct mosquitto_message* msg)
{
    if (userdata) static_cast<MqttPublisher*>(userdata)->handle_message(msg);
}

void MqttPublisher::handle_message(const struct mosquitto_message* msg)
{
    if (!msg || !msg->payload || !msg->topic) return;
    const std::string topic(msg->topic);
    const std::string payload(static_cast<const char*>(msg->payload),
                              static_cast<std::size_t>(msg->payloadlen));

    if (topic == history_req_topic_) {
        handle_history_request(payload);
        return;
    }

    if (topic != config_topic_set_) return;

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(payload);
    } catch (...) {
        std::println(stderr, "[MqttPublisher] config/set: invalid JSON");
        return;
    }

    if (!j.contains("sensor_id") || !j.contains("threshold_warn") || !j.contains("threshold_crit")) {
        std::println(stderr, "[MqttPublisher] config/set: missing fields");
        return;
    }

    const auto  id   = j["sensor_id"].get<std::string>();
    const float warn = j["threshold_warn"].get<float>();
    const float crit = j["threshold_crit"].get<float>();

    if (warn <= 0.0f || crit <= 0.0f || warn >= crit) {
        std::println(stderr, "[MqttPublisher] config/set: invalid thresholds for '{}'", id);
        return;
    }

    if (threshold_cb_) {
        if (auto r = threshold_cb_(id, warn, crit); !r)
            std::println(stderr, "[MqttPublisher] config/set update failed: {}", r.error());
        else
            std::println("[MqttPublisher] Thresholds updated: {} warn={} crit={}", id, warn, crit);
    }
}

std::string MqttPublisher::build_history_response(const std::string& request_payload) const
{
    nlohmann::json req;
    try {
        req = nlohmann::json::parse(request_payload);
    } catch (...) {
        return {};
    }

    if (!req.contains("request_id") || !req.contains("sensor_id")) return {};
    const auto request_id = req["request_id"].get<std::string>();
    const auto sensor_id  = req["sensor_id"].get<std::string>();

    constexpr int kHardCap = 500;
    int limit = req.contains("limit") ? req["limit"].get<int>() : 240;
    limit = std::clamp(limit, 1, kHardCap);

    std::vector<StoredPoint> points;
    if (history_store_) {
        if (req.contains("since_ts") && req["since_ts"].is_number_integer()) {
            points = history_store_->since(sensor_id, req["since_ts"].get<int64_t>(), limit);
        } else {
            points = history_store_->recent(sensor_id, limit);
        }
    }

    std::string metric;
    if (history_store_) {
        if (auto m = history_store_->metric_for(sensor_id)) metric = std::move(*m);
    }

    nlohmann::json out_points = nlohmann::json::array();
    for (const auto& p : points) {
        out_points.push_back({{"ts", p.ts_ms}, {"value", p.value}});
    }

    nlohmann::json resp = {
        {"request_id", request_id},
        {"sensor_id",  sensor_id},
        {"metric",     metric},
        {"points",     std::move(out_points)},
        {"truncated",  static_cast<int>(points.size()) >= limit},
    };
    return resp.dump();
}

void MqttPublisher::handle_history_request(const std::string& payload)
{
    const std::string body = build_history_response(payload);
    if (body.empty()) {
        std::println(stderr, "[MqttPublisher] history/req: invalid request");
        return;
    }
    nlohmann::json j;
    try { j = nlohmann::json::parse(body); } catch (...) { return; }

    const std::string topic = history_resp_prefix_ + j["request_id"].get<std::string>();
    publish(topic, body, /*retain=*/false);
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
