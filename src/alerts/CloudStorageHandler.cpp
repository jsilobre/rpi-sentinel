#ifdef ENABLE_CLOUD_STORAGE

#include "CloudStorageHandler.hpp"
#include "../events/SensorEvent.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <format>
#include <mutex>
#include <print>
#include <queue>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace rpi {

namespace {

constexpr std::size_t QUEUE_CAPACITY   = 1000;
constexpr std::size_t BATCH_SIZE       = 50;
constexpr long        CURL_TIMEOUT_S   = 10;
constexpr int         DRAIN_WAIT_MS    = 2000;

std::string resolve_api_key(const CloudStorageConfig& cfg)
{
    if (!cfg.api_key_env.empty()) {
        if (const char* v = std::getenv(cfg.api_key_env.c_str()); v && *v != '\0')
            return std::string{v};
    }
    return cfg.api_key;
}

// Discard response body — only HTTP status matters.
std::size_t discard_write(char*, std::size_t size, std::size_t nmemb, void*) noexcept
{
    return size * nmemb;
}

} // namespace

struct QueueItem {
    std::string sensor_id;
    std::string metric;
    float       value{};
    std::int64_t ts_ms{};
};

struct CloudStorageHandler::Impl {
    std::string             ingest_url;
    std::string             api_key;

    std::queue<QueueItem>   queue;
    std::mutex              mu;
    std::condition_variable cv;
    std::atomic<bool>       overflow_logged{false};

    std::jthread            worker;

    void run(std::stop_token stop)
    {
        CURL* curl = curl_easy_init();
        if (!curl) {
            std::println(stderr, "[CloudStorage] curl_easy_init failed; worker exiting");
            return;
        }

        const std::string auth_header = std::format("Authorization: Bearer {}", api_key);
        curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        headers = curl_slist_append(headers, auth_header.c_str());

        curl_easy_setopt(curl, CURLOPT_HTTPHEADER,    headers);
        curl_easy_setopt(curl, CURLOPT_URL,           ingest_url.c_str());
        curl_easy_setopt(curl, CURLOPT_POST,          1L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT,       CURL_TIMEOUT_S);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard_write);

        while (!stop.stop_requested()) {
            std::vector<QueueItem> batch;
            batch.reserve(BATCH_SIZE);

            {
                std::unique_lock lk{mu};
                cv.wait_for(lk,
                    std::chrono::milliseconds{DRAIN_WAIT_MS},
                    [&]{ return !queue.empty() || stop.stop_requested(); });

                while (!queue.empty() && batch.size() < BATCH_SIZE) {
                    batch.push_back(std::move(queue.front()));
                    queue.pop();
                }
            }

            if (batch.empty()) continue;
            post_batch(curl, batch);
        }

        // Flush remaining items on graceful shutdown.
        std::vector<QueueItem> remaining;
        {
            std::lock_guard lk{mu};
            while (!queue.empty()) {
                remaining.push_back(std::move(queue.front()));
                queue.pop();
            }
        }
        while (!remaining.empty()) {
            std::vector<QueueItem> flush;
            const std::size_t take = std::min(remaining.size(), BATCH_SIZE);
            flush.assign(
                std::make_move_iterator(remaining.begin()),
                std::make_move_iterator(remaining.begin() + static_cast<std::ptrdiff_t>(take)));
            remaining.erase(remaining.begin(),
                            remaining.begin() + static_cast<std::ptrdiff_t>(take));
            post_batch(curl, flush);
        }

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    }

    void post_batch(CURL* curl, const std::vector<QueueItem>& batch)
    {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& item : batch) {
            arr.push_back({
                {"sensor_id", item.sensor_id},
                {"metric",    item.metric},
                {"value",     item.value},
                {"ts",        item.ts_ms},
            });
        }
        const std::string body = arr.dump();

        curl_easy_setopt(curl, CURLOPT_POSTFIELDS,    body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));

        const CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            std::println(stderr, "[CloudStorage] POST failed ({}): {}",
                         batch.size(), curl_easy_strerror(res));
            return;
        }
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        if (http_code != 200) {
            std::println(stderr, "[CloudStorage] POST returned HTTP {} ({} items)",
                         http_code, batch.size());
        }
    }
};

CloudStorageHandler::CloudStorageHandler(const CloudStorageConfig& cfg)
    : impl_{std::make_unique<Impl>()}
{
    if (cfg.endpoint.empty())
        throw std::runtime_error{"CloudStorageHandler: endpoint is empty"};

    impl_->api_key = resolve_api_key(cfg);
    if (impl_->api_key.empty())
        throw std::runtime_error{std::format(
            "CloudStorageHandler: API key not found "
            "(env '{}' unset and api_key empty)", cfg.api_key_env)};

    impl_->ingest_url = cfg.endpoint + "/ingest";

    impl_->worker = std::jthread{[this](std::stop_token st) {
        impl_->run(std::move(st));
    }};

    std::println("[CloudStorage] Handler initialized: endpoint={}", cfg.endpoint);
}

CloudStorageHandler::~CloudStorageHandler()
{
    // jthread destructor calls request_stop() + join() automatically.
    // Notify so the worker wakes from wait_for immediately.
    impl_->cv.notify_all();
}

void CloudStorageHandler::on_event(const SensorEvent& event)
{
    if (event.type != SensorEvent::Type::Reading) return;

    const auto ts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        event.timestamp.time_since_epoch()).count();

    std::lock_guard lk{impl_->mu};
    if (impl_->queue.size() >= QUEUE_CAPACITY) {
        impl_->queue.pop(); // drop oldest
        if (!impl_->overflow_logged.exchange(true))
            std::println(stderr, "[CloudStorage] Queue full; dropping oldest item");
        return;
    }
    impl_->overflow_logged.store(false);
    impl_->queue.push({
        .sensor_id = event.sensor_id,
        .metric    = event.metric,
        .value     = event.value,
        .ts_ms     = ts_ms,
    });
    impl_->cv.notify_one();
}

} // namespace rpi

#endif // ENABLE_CLOUD_STORAGE
