#pragma once

#ifdef ENABLE_CLOUD_STORAGE

#include "IAlertHandler.hpp"
#include "../monitoring/Config.hpp"

#include <memory>

namespace rpi {

// IAlertHandler that ships every SensorEvent::Reading as a JSON batch
// to a Cloudflare Worker via HTTP POST (libcurl).
//
// ThresholdExceeded / ThresholdRecovered events are intentionally not
// forwarded; the cloud store is a time-series archive, not an alerting
// channel.
//
// on_event() is non-blocking: it enqueues a record into a bounded
// std::queue protected by std::mutex + std::condition_variable.  A
// dedicated std::jthread drains the queue in batches and POSTs via
// libcurl. When the queue reaches QUEUE_CAPACITY the oldest item is
// dropped rather than blocking.
//
// Construction throws std::runtime_error if endpoint is empty or the
// API key cannot be resolved. on_event() never throws.
//
// curl_global_init() must have been called before constructing this
// object (call it once in main() before starting any threads).
class CloudStorageHandler final : public IAlertHandler {
public:
    explicit CloudStorageHandler(const CloudStorageConfig& cfg);
    ~CloudStorageHandler() override;

    CloudStorageHandler(const CloudStorageHandler&)            = delete;
    CloudStorageHandler& operator=(const CloudStorageHandler&) = delete;
    CloudStorageHandler(CloudStorageHandler&&)                 = delete;
    CloudStorageHandler& operator=(CloudStorageHandler&&)      = delete;

    void on_event(const SensorEvent& event) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace rpi

#endif // ENABLE_CLOUD_STORAGE
