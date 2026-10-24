#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>

namespace rpi {

// Raw byte transport to one I2C device. Abstracted so tests can fake it.
class II2cBus {
public:
    virtual ~II2cBus() = default;

    virtual auto write(std::span<const std::uint8_t> data) -> bool = 0;
    virtual auto read(std::span<std::uint8_t> data) -> bool = 0;
    // Command execution time between a write and the following read.
    virtual void wait(std::chrono::milliseconds duration) = 0;
};

// /dev/i2c-N, addressed with ioctl(I2C_SLAVE). Opened lazily, so a bus that
// appears after start-up (module loaded late) is picked up on the next write.
class LinuxI2cBus final : public II2cBus {
public:
    LinuxI2cBus(std::string device_path, std::uint8_t address);
    ~LinuxI2cBus() override;
    LinuxI2cBus(const LinuxI2cBus&)            = delete;
    LinuxI2cBus& operator=(const LinuxI2cBus&) = delete;

    auto write(std::span<const std::uint8_t> data) -> bool override;
    auto read(std::span<std::uint8_t> data) -> bool override;
    void wait(std::chrono::milliseconds duration) override;

private:
    auto ensure_open() -> bool;

    std::string  device_path_;
    std::uint8_t address_;
    int          fd_ = -1;
};

struct Sgp30Options {
    // Where the IAQ baseline is persisted (empty: never saved/restored).
    std::string baseline_path        = {};
    // IIO directory of a DHT11 (in_temp_input + in_humidityrelative_input)
    // used for humidity compensation (empty: no compensation).
    std::string humidity_device_path = {};
};

struct Sgp30Sample {
    std::uint16_t eco2_ppm = 0;
    std::uint16_t tvoc_ppb = 0;
};

// Drives one SGP30 from user space, as the datasheet requires: measure_iaq
// exactly once per second (the on-chip baseline algorithm depends on it),
// baseline restored at start and saved hourly, absolute humidity fed from a
// companion sensor. Shared by the eco2 and tvoc readers of the same chip.
class Sgp30Device {
public:
    Sgp30Device(std::unique_ptr<II2cBus> bus, Sgp30Options options);
    ~Sgp30Device();
    Sgp30Device(const Sgp30Device&)            = delete;
    Sgp30Device& operator=(const Sgp30Device&) = delete;

    // One device per I2C bus path, started on first use.
    static auto acquire(const std::string& bus_path, const Sgp30Options& options)
        -> std::shared_ptr<Sgp30Device>;

    // Fills options left empty by an earlier user of the same chip.
    void merge_options(const Sgp30Options& options);

    // Latest measurement, or nullopt while warming up or if the chip stopped
    // answering.
    auto latest() const -> std::optional<Sgp30Sample>;

    // ── Driven by the 1 Hz thread; public for tests ──
    auto init() -> bool;   // iaq_init + baseline restore + humidity
    void step();           // one 1 s cycle: measure, then periodic tasks

    void start();
    void stop();

private:
    auto send(std::uint16_t command, std::span<const std::uint16_t> words = {}) -> bool;
    auto receive(std::span<std::uint16_t> words) -> bool;
    void update_humidity();
    void save_baseline();

    std::unique_ptr<II2cBus> bus_;

    mutable std::mutex mutex_;   // guards options_ and latest_
    Sgp30Options       options_;
    std::optional<Sgp30Sample> latest_;

    bool          initialized_       = false;
    bool          baseline_restored_ = false;
    std::uint64_t measurements_      = 0;   // since the last iaq_init
    int           consecutive_errors_ = 0;

    std::mutex                  sleep_mutex_;
    std::condition_variable_any sleep_cv_;
    std::jthread                thread_;
};

} // namespace rpi
