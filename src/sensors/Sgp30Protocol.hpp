#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>

// Pure helpers for the Sensirion SGP30 I2C protocol (datasheet v1.0).
namespace rpi::sgp30 {

inline constexpr std::uint8_t kI2cAddress = 0x58;

inline constexpr std::uint16_t kCmdIaqInit             = 0x2003;
inline constexpr std::uint16_t kCmdMeasureIaq          = 0x2008;
inline constexpr std::uint16_t kCmdGetIaqBaseline      = 0x2015;
inline constexpr std::uint16_t kCmdSetIaqBaseline      = 0x201e;
inline constexpr std::uint16_t kCmdSetAbsoluteHumidity = 0x2061;

// After iaq_init the sensor reports fixed 400 ppm / 0 ppb for ~15 s.
inline constexpr int kWarmupMeasurements = 15;
// Without a restored baseline, the algorithm needs 12 h before its baseline
// is worth saving.
inline constexpr std::chrono::hours kBaselineWarmup{12};
// Sensirion: persist the baseline hourly; a stored one is valid for 7 days.
inline constexpr std::chrono::hours kBaselineSaveInterval{1};
inline constexpr std::chrono::hours kBaselineMaxAge{24 * 7};

// CRC-8, polynomial 0x31, init 0xFF (datasheet §6.6). crc8({0xBE, 0xEF}) == 0x92.
auto crc8(std::span<const std::uint8_t> data) -> std::uint8_t;

// Absolute humidity in g/m³ from temperature (°C) and relative humidity (%).
auto absolute_humidity(float temp_c, float rh_pct) -> float;

// Absolute humidity in the 8.8 fixed-point format of set_absolute_humidity.
// Never returns 0, which would disable the compensation.
auto absolute_humidity_fixed(float temp_c, float rh_pct) -> std::uint16_t;

struct Baseline {
    std::uint16_t eco2     = 0;
    std::uint16_t tvoc     = 0;
    std::int64_t  saved_at = 0;   // Unix seconds
};

// Returns the stored baseline if the file exists, parses and is younger
// than kBaselineMaxAge relative to `now`.
auto load_baseline(const std::filesystem::path& path,
                   std::chrono::system_clock::time_point now) -> std::optional<Baseline>;

// Writes atomically (tmp file + rename), creating the parent directory.
auto save_baseline(const std::filesystem::path& path, const Baseline& b) -> bool;

} // namespace rpi::sgp30
