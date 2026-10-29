#include "Sgp30Protocol.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>

namespace rpi::sgp30 {

auto crc8(std::span<const std::uint8_t> data) -> std::uint8_t
{
    std::uint8_t crc = 0xFF;
    for (auto byte : data) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 0x80) ? static_cast<std::uint8_t>((crc << 1) ^ 0x31)
                               : static_cast<std::uint8_t>(crc << 1);
    }
    return crc;
}

auto absolute_humidity(float temp_c, float rh_pct) -> float
{
    // Magnus formula, as given in the SGP30 driver integration guide.
    const float saturation_hpa = 6.112f * std::exp(17.62f * temp_c / (243.12f + temp_c));
    return 216.7f * (rh_pct / 100.0f * saturation_hpa / (273.15f + temp_c));
}

auto absolute_humidity_fixed(float temp_c, float rh_pct) -> std::uint16_t
{
    const float fixed = std::round(absolute_humidity(temp_c, rh_pct) * 256.0f);
    return static_cast<std::uint16_t>(std::clamp(fixed, 1.0f, 65535.0f));
}

auto load_baseline(const std::filesystem::path& path,
                   std::chrono::system_clock::time_point now) -> std::optional<Baseline>
{
    std::ifstream f(path);
    if (!f.is_open()) return std::nullopt;

    unsigned eco2{}, tvoc{};
    std::int64_t saved_at{};
    if (!(f >> eco2 >> tvoc >> saved_at)) return std::nullopt;
    if (eco2 == 0 || tvoc == 0 || eco2 > 0xFFFF || tvoc > 0xFFFF) return std::nullopt;

    const auto saved = std::chrono::system_clock::time_point{std::chrono::seconds{saved_at}};
    if (saved > now || now - saved > kBaselineMaxAge) return std::nullopt;

    return Baseline{
        .eco2     = static_cast<std::uint16_t>(eco2),
        .tvoc     = static_cast<std::uint16_t>(tvoc),
        .saved_at = saved_at,
    };
}

auto save_baseline(const std::filesystem::path& path, const Baseline& b) -> bool
{
    std::error_code ec;
    if (path.has_parent_path())
        std::filesystem::create_directories(path.parent_path(), ec);

    auto tmp = path;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f.is_open()) return false;
        f << b.eco2 << ' ' << b.tvoc << ' ' << b.saved_at << '\n';
        if (!f) return false;
    }
    std::filesystem::rename(tmp, path, ec);
    return !ec;
}

} // namespace rpi::sgp30
