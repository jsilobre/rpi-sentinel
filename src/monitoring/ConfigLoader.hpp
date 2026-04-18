#pragma once

#include "Config.hpp"
#include <expected>
#include <filesystem>
#include <string>

namespace rpi {

// Loads configuration from a JSON file.
// Returns Config populated from the file, or an error description.
// Fields absent from the JSON fall back to their default values (defined in Config).
auto load_config(const std::filesystem::path& path) -> std::expected<Config, std::string>;

} // namespace rpi
