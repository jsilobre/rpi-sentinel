#pragma once

#include "Config.hpp"
#include <expected>
#include <filesystem>
#include <string>

namespace rpi {

// Charge la configuration depuis un fichier JSON.
// Retourne Config avec les valeurs du fichier, ou une description d'erreur.
// Les champs absents du JSON prennent leur valeur par défaut (définie dans Config).
auto load_config(const std::filesystem::path& path) -> std::expected<Config, std::string>;

} // namespace rpi
