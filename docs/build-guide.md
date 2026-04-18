# Guide de build — rpi-temp-monitor

## Prérequis

| Outil | Version minimale | Remarque |
|---|---|---|
| CMake | 3.28 | `cmake --version` |
| GCC | 14 | `g++-14 --version` — requis pour `<print>` (C++23) |
| Git | toute | |
| ninja-build | toute | optionnel mais recommandé |

**Ubuntu 24.04 :**
```bash
sudo apt-get install -y g++-14 cmake ninja-build
```

---

## Structure du build system

```
CMakeLists.txt                 ← racine : projet, standard C++23, options
cmake/
  CompilerOptions.cmake        ← fonction apply_compiler_options(target)
                                  flags : -Wall -Wextra -Wpedantic -Werror
src/
  CMakeLists.txt               ← sous-répertoires + exécutable rpi-temp-monitor
  alerts/CMakeLists.txt        ← lib statique : alerts
  events/CMakeLists.txt        ← lib statique : events  (dépend de alerts)
  sensors/CMakeLists.txt       ← lib statique : sensors
  monitoring/CMakeLists.txt    ← lib statique : monitoring (dépend de sensors, events)
tests/
  CMakeLists.txt               ← FetchContent GTest + exécutable rpi_tests
```

**Graphe de dépendances CMake :**

```
rpi-temp-monitor
  └─ monitoring
       ├─ sensors
       └─ events
            └─ alerts
```

---

## Commandes de build

### Configuration

```bash
# Avec tests (défaut)
cmake -B build -DCMAKE_CXX_COMPILER=g++-14 -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON

# Sans tests
cmake -B build -DCMAKE_CXX_COMPILER=g++-14 -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF

# Avec Ninja (plus rapide)
cmake -B build -G Ninja -DCMAKE_CXX_COMPILER=g++-14 -DCMAKE_BUILD_TYPE=Release
```

### Compilation

```bash
cmake --build build --parallel
# ou avec Ninja :
ninja -C build
```

### Lancer les tests

```bash
ctest --test-dir build --output-on-failure
```

### Lancer l'application

```bash
./build/rpi-temp-monitor
```

Sortie attendue (capteur simulé) :
```
[main] Monitor started. Press Ctrl+C to stop.
[2025-04-18 14:32:01] [EXCEEDED]  sensor=sim-0 temp=68.3°C threshold=65.0°C
[2025-04-18 14:32:03] [RECOVERED] sensor=sim-0 temp=62.8°C threshold=65.0°C
```

---

## Options CMake

| Option | Défaut | Description |
|---|---|---|
| `BUILD_TESTING` | `ON` | Active la compilation et l'enregistrement des tests Google Test |
| `CMAKE_BUILD_TYPE` | — | `Release` / `Debug` / `RelWithDebInfo` |
| `CMAKE_EXPORT_COMPILE_COMMANDS` | `ON` | Génère `build/compile_commands.json` pour clangd / VSCode |

---

## Support LSP (clangd / VSCode)

Le fichier `build/compile_commands.json` est généré automatiquement.

```bash
# Lien symbolique pour que clangd le trouve à la racine
ln -s build/compile_commands.json compile_commands.json
```

---

## CI/CD — GitHub Actions

Le workflow `.github/workflows/ci.yml` se déclenche sur chaque push et pull request.

### Étapes

```
1. Checkout du code
2. Installation de GCC 14 + CMake + Ninja
3. cmake configure  (-DBUILD_TESTING=ON)
4. cmake build      (--parallel)
5. ctest            (--output-on-failure)
```

### Exigences pour merger

- Le build doit passer sans erreur de compilation (flags `-Werror` actifs).
- Les 4 tests unitaires doivent passer.

---

## Portage sur Raspberry Pi 5

### Option A — Build natif sur le RPi

Installer les outils directement sur le RPi (Raspberry Pi OS 64-bit, basé Debian Bookworm) :

```bash
sudo apt-get install -y g++-14 cmake ninja-build git
git clone <repo>
cd rpi-temp-monitor
cmake -B build -DCMAKE_CXX_COMPILER=g++-14 -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

### Option B — Cross-compilation (PC → ARM64)

```bash
sudo apt-get install -y gcc-14-aarch64-linux-gnu g++-14-aarch64-linux-gnu

cmake -B build-arm \
  -DCMAKE_SYSTEM_NAME=Linux \
  -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
  -DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++-14 \
  -DBUILD_TESTING=OFF \
  -DCMAKE_BUILD_TYPE=Release

cmake --build build-arm --parallel
# Copier build-arm/rpi-temp-monitor sur le RPi via scp
```

### Activation du bus 1-Wire sur RPi 5

Ajouter dans `/boot/firmware/config.txt` :

```
dtoverlay=w1-gpio,gpiopin=4
```

Redémarrer, puis vérifier :

```bash
ls /sys/bus/w1/devices/
# 28-xxxxxxxxxx   ← identifiant du DS18B20

cat /sys/bus/w1/devices/28-xxxxxxxxxx/temperature
# 23562  ← millidegrés Celsius (= 23.562 °C)
```

Configurer le `device_path` dans `main.cpp` :

```cpp
Config config{
    .sensor_type = rpi::SensorType::DS18B20,
    .device_path = "/sys/bus/w1/devices/28-xxxxxxxxxx/temperature",
    ...
};
```

---

## Ajouter un test

Les tests se trouvent dans `tests/`. Ils utilisent Google Test (téléchargé via `FetchContent`
au moment du `cmake configure`).

```cpp
// tests/test_mon_composant.cpp
#include <gtest/gtest.h>
#include "../src/mon_composant/MonComposant.hpp"

TEST(MonComposant, DescriptionDuCas)
{
    // Arrange
    // Act
    // Assert
    EXPECT_EQ(..., ...);
}
```

Enregistrer dans `tests/CMakeLists.txt` :

```cmake
add_executable(rpi_tests
    test_event_bus.cpp
    test_threshold_monitor.cpp
    test_mon_composant.cpp   # ← ajouter ici
)
```
