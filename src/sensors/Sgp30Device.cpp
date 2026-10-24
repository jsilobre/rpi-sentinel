#include "Sgp30Device.hpp"
#include "Sgp30Protocol.hpp"

#include <array>
#include <fcntl.h>
#include <fstream>
#include <linux/i2c-dev.h>
#include <map>
#include <print>
#include <sys/ioctl.h>
#include <unistd.h>
#include <vector>

namespace rpi {

using namespace std::chrono_literals;

// ── LinuxI2cBus ──────────────────────────────────────────────────────────────

LinuxI2cBus::LinuxI2cBus(std::string device_path, std::uint8_t address)
    : device_path_(std::move(device_path))
    , address_(address)
{}

auto LinuxI2cBus::ensure_open() -> bool
{
    if (fd_ >= 0) return true;
    fd_ = ::open(device_path_.c_str(), O_RDWR | O_CLOEXEC);
    if (fd_ >= 0 && ::ioctl(fd_, I2C_SLAVE, static_cast<unsigned long>(address_)) < 0) {
        ::close(fd_);
        fd_ = -1;
    }
    return fd_ >= 0;
}

LinuxI2cBus::~LinuxI2cBus()
{
    if (fd_ >= 0) ::close(fd_);
}

auto LinuxI2cBus::write(std::span<const std::uint8_t> data) -> bool
{
    return ensure_open() && ::write(fd_, data.data(), data.size()) == static_cast<ssize_t>(data.size());
}

auto LinuxI2cBus::read(std::span<std::uint8_t> data) -> bool
{
    return fd_ >= 0 && ::read(fd_, data.data(), data.size()) == static_cast<ssize_t>(data.size());
}

void LinuxI2cBus::wait(std::chrono::milliseconds duration)
{
    std::this_thread::sleep_for(duration);
}

// ── Sgp30Device ──────────────────────────────────────────────────────────────

namespace {

// After this many failed measurements in a row the chip is re-initialised
// (e.g. it lost power), which also restores the saved baseline.
constexpr int kMaxConsecutiveErrors = 10;
constexpr std::uint64_t kHumidityUpdateEvery = 60;   // measurements (= seconds)

auto measurements_in(std::chrono::seconds d) -> std::uint64_t
{
    return static_cast<std::uint64_t>(d.count());
}

} // namespace

Sgp30Device::Sgp30Device(std::unique_ptr<II2cBus> bus, Sgp30Options options)
    : bus_(std::move(bus))
    , options_(std::move(options))
{}

Sgp30Device::~Sgp30Device()
{
    stop();
}

auto Sgp30Device::acquire(const std::string& bus_path, const Sgp30Options& options)
    -> std::shared_ptr<Sgp30Device>
{
    static std::mutex registry_mutex;
    static std::map<std::string, std::weak_ptr<Sgp30Device>> registry;

    std::lock_guard lock(registry_mutex);
    if (auto existing = registry[bus_path].lock()) {
        existing->merge_options(options);
        return existing;
    }
    auto device = std::make_shared<Sgp30Device>(
        std::make_unique<LinuxI2cBus>(bus_path, sgp30::kI2cAddress), options);
    device->start();
    registry[bus_path] = device;
    return device;
}

void Sgp30Device::merge_options(const Sgp30Options& options)
{
    std::lock_guard lock(mutex_);
    if (options_.baseline_path.empty())
        options_.baseline_path = options.baseline_path;
    if (options_.humidity_device_path.empty())
        options_.humidity_device_path = options.humidity_device_path;
}

auto Sgp30Device::latest() const -> std::optional<Sgp30Sample>
{
    std::lock_guard lock(mutex_);
    return latest_;
}

auto Sgp30Device::send(std::uint16_t command, std::span<const std::uint16_t> words) -> bool
{
    std::vector<std::uint8_t> buf{
        static_cast<std::uint8_t>(command >> 8),
        static_cast<std::uint8_t>(command & 0xFF),
    };
    for (auto w : words) {
        const std::array<std::uint8_t, 2> bytes{
            static_cast<std::uint8_t>(w >> 8),
            static_cast<std::uint8_t>(w & 0xFF),
        };
        buf.insert(buf.end(), bytes.begin(), bytes.end());
        buf.push_back(sgp30::crc8(bytes));
    }
    return bus_->write(buf);
}

auto Sgp30Device::receive(std::span<std::uint16_t> words) -> bool
{
    std::vector<std::uint8_t> buf(words.size() * 3);
    if (!bus_->read(buf)) return false;
    for (std::size_t i = 0; i < words.size(); ++i) {
        const std::span<const std::uint8_t> bytes(buf.data() + i * 3, 2);
        if (sgp30::crc8(bytes) != buf[i * 3 + 2]) return false;
        words[i] = static_cast<std::uint16_t>((bytes[0] << 8) | bytes[1]);
    }
    return true;
}

auto Sgp30Device::init() -> bool
{
    {
        std::lock_guard lock(mutex_);
        latest_.reset();
    }
    measurements_       = 0;
    consecutive_errors_ = 0;
    baseline_restored_  = false;

    if (!send(sgp30::kCmdIaqInit)) {
        initialized_ = false;
        return false;
    }
    bus_->wait(10ms);

    std::string baseline_path;
    {
        std::lock_guard lock(mutex_);
        baseline_path = options_.baseline_path;
    }
    if (!baseline_path.empty()) {
        if (auto b = sgp30::load_baseline(baseline_path, std::chrono::system_clock::now())) {
            // set_iaq_baseline takes the words in reverse order: TVOC first.
            const std::array<std::uint16_t, 2> words{b->tvoc, b->eco2};
            baseline_restored_ = send(sgp30::kCmdSetIaqBaseline, words);
            bus_->wait(10ms);
            if (baseline_restored_)
                std::println("[SGP30] baseline restored (eco2=0x{:04x} tvoc=0x{:04x})",
                             b->eco2, b->tvoc);
        } else {
            std::println("[SGP30] no valid baseline in {}: 12 h early-operation phase",
                         baseline_path);
        }
    }

    update_humidity();
    initialized_ = true;
    return true;
}

void Sgp30Device::step()
{
    if (!initialized_) return;

    std::array<std::uint16_t, 2> words{};
    bool ok = send(sgp30::kCmdMeasureIaq);
    bus_->wait(12ms);
    ok = ok && receive(words);

    if (!ok) {
        if (++consecutive_errors_ >= kMaxConsecutiveErrors) {
            std::println("[SGP30] {} failed measurements, re-initialising", consecutive_errors_);
            std::lock_guard lock(mutex_);
            latest_.reset();
            initialized_ = false;
        }
        return;
    }
    consecutive_errors_ = 0;
    ++measurements_;

    if (measurements_ > static_cast<std::uint64_t>(sgp30::kWarmupMeasurements)) {
        std::lock_guard lock(mutex_);
        latest_ = Sgp30Sample{.eco2_ppm = words[0], .tvoc_ppb = words[1]};
    }

    if (measurements_ % kHumidityUpdateEvery == 0)
        update_humidity();

    // A fresh chip's baseline is only meaningful after the 12 h early-operation
    // phase; a restored one can be refreshed from the first hour on.
    if (measurements_ % measurements_in(sgp30::kBaselineSaveInterval) == 0
        && (baseline_restored_ || measurements_ >= measurements_in(sgp30::kBaselineWarmup)))
        save_baseline();
}

void Sgp30Device::update_humidity()
{
    std::string dir;
    {
        std::lock_guard lock(mutex_);
        dir = options_.humidity_device_path;
    }
    if (dir.empty()) return;

    // DHT11 IIO driver: milli-degrees and milli-percent.
    int temp_milli{}, rh_milli{};
    std::ifstream t(dir + "/in_temp_input");
    std::ifstream h(dir + "/in_humidityrelative_input");
    if (!(t >> temp_milli) || !(h >> rh_milli)) return;   // keep the last value
    if (rh_milli <= 0 || rh_milli > 100'000) return;

    const auto fixed = sgp30::absolute_humidity_fixed(
        static_cast<float>(temp_milli) / 1000.0f, static_cast<float>(rh_milli) / 1000.0f);
    const std::array<std::uint16_t, 1> words{fixed};
    send(sgp30::kCmdSetAbsoluteHumidity, words);
    bus_->wait(10ms);
}

void Sgp30Device::save_baseline()
{
    std::string path;
    {
        std::lock_guard lock(mutex_);
        path = options_.baseline_path;
    }
    if (path.empty()) return;

    std::array<std::uint16_t, 2> words{};
    bool ok = send(sgp30::kCmdGetIaqBaseline);
    bus_->wait(10ms);
    if (!ok || !receive(words)) return;

    const auto now = std::chrono::system_clock::now().time_since_epoch();
    sgp30::save_baseline(path, sgp30::Baseline{
        .eco2     = words[0],
        .tvoc     = words[1],
        .saved_at = std::chrono::duration_cast<std::chrono::seconds>(now).count(),
    });
}

void Sgp30Device::start()
{
    thread_ = std::jthread([this](std::stop_token st) {
        auto sleep_until = [&](std::chrono::steady_clock::time_point deadline) {
            std::unique_lock lock(sleep_mutex_);
            sleep_cv_.wait_until(lock, st, deadline, [] { return false; });
        };

        bool reported_missing = false;
        auto next = std::chrono::steady_clock::now();
        while (!st.stop_requested()) {
            if (!initialized_ && !init()) {
                if (!reported_missing)
                    std::println("[SGP30] chip not answering on I2C, retrying every 10 s");
                reported_missing = true;
                sleep_until(std::chrono::steady_clock::now() + 10s);
                next = std::chrono::steady_clock::now();
                continue;
            }
            reported_missing = false;

            step();
            next += 1s;
            // Fall back on schedule rather than bursting after a stall.
            if (const auto now = std::chrono::steady_clock::now(); now > next + 5s)
                next = now;
            sleep_until(next);
        }
    });
}

void Sgp30Device::stop()
{
    if (thread_.joinable()) {
        thread_.request_stop();
        thread_.join();
    }
}

} // namespace rpi
