# Architecture — rpi-temp-monitor

## 1. Principes directeurs

- **Séparation des responsabilités** : chaque couche ne connaît que ses voisines immédiates via des interfaces abstraites.
- **Testabilité** : le capteur réel est remplaçable par un simulateur sans modifier le code métier.
- **Extensibilité** : ajouter un capteur ou un type d'alerte n'implique qu'une nouvelle classe implémentant une interface existante.
- **Gestion d'erreurs sans exceptions** : `std::expected<T, E>` est utilisé à la frontière matérielle.

---

## 2. Vue en couches

```
┌─────────────────────────────────────────────────────────┐
│                        main.cpp                         │
│  (fabrique le capteur, l'EventBus, les handlers,        │
│   le ThresholdMonitor et orchestre l'arrêt propre)      │
└────────────┬───────────────────────────────┬────────────┘
             │                               │
             ▼                               ▼
┌────────────────────┐           ┌───────────────────────┐
│   MONITORING       │           │   ALERTS              │
│  ThresholdMonitor  │──────────►│  IAlertHandler        │
│  Config            │  dispatch │  LogAlert             │
└────────┬───────────┘  event    └───────────────────────┘
         │                                ▲
         │ read()                         │ via EventBus
         ▼                               │
┌────────────────────┐           ┌───────────────────────┐
│   SENSORS          │           │   EVENTS              │
│  ISensorReader     │           │  ThermalEvent         │
│  DS18B20Reader     │           │  EventBus             │
│  SimulatedSensor   │           └───────────────────────┘
└────────────────────┘
```

### Règle de dépendance

```
main → monitoring → sensors
                  → events → alerts
```

Aucune couche basse ne dépend d'une couche haute.

---

## 3. Composants

### 3.1 Couche `sensors`

| Fichier | Rôle |
|---|---|
| `ISensorReader.hpp` | Interface abstraite. Définit `read()` et `sensor_id()`. |
| `DS18B20Reader.hpp/cpp` | Lit `/sys/bus/w1/devices/<id>/temperature` (noyau Linux 1-Wire). |
| `SimulatedSensor.hpp/cpp` | Capteur piloté par une fonction génératrice (`std::function<float()>`). Fournit une sinusoïde par défaut. |

**Interface centrale :**

```cpp
class ISensorReader {
public:
    virtual auto read()      -> std::expected<SensorReading, SensorError> = 0;
    virtual auto sensor_id() -> std::string = 0;
};
```

`std::expected` transporte soit une mesure valide soit un code d'erreur (`DeviceNotFound`, `ReadFailure`, `ParseError`) sans lever d'exception.

**Ajout d'un nouveau capteur :**
1. Créer une classe qui hérite de `ISensorReader`.
2. L'ajouter à `SensorType` dans `Config.hpp`.
3. L'instancier dans la factory `make_sensor()` de `main.cpp`.

---

### 3.2 Couche `events`

| Fichier | Rôle |
|---|---|
| `ThermalEvent.hpp` | Structure de données d'un événement : type, température mesurée, seuil franchi, id du capteur, horodatage. |
| `EventBus.hpp/cpp` | Registre de handlers. Dispatch synchrone protégé par un `std::mutex`. |

**Types d'événements :**

| `ThermalEvent::Type` | Signification |
|---|---|
| `ThresholdExceeded` | La température a dépassé un seuil (warn ou crit). |
| `ThresholdRecovered` | La température est redescendue sous `seuil − hystérésis`. |

---

### 3.3 Couche `monitoring`

| Fichier | Rôle |
|---|---|
| `Config.hpp` | Paramètres : type de capteur, seuils warn/crit, hystérésis, intervalle de polling. |
| `ThresholdMonitor.hpp/cpp` | Boucle de polling dans un `std::jthread`. Compare la température aux seuils et dispatche les événements. |

**Gestion de l'hystérésis :**

```
température
    │      ┌──────────────── threshold_crit ──────────────────
    │      │  ← EXCEEDED dispatché                            │
    │      │                                         ← RECOVERED dispatché
    │      └──────────────── threshold_crit − hysteresis ─────
    └─ temps
```

Sans hystérésis, un capteur oscillant autour du seuil génèrerait un événement à chaque cycle.
L'état `warn_active_` / `crit_active_` est maintenu entre les cycles de polling.

---

### 3.4 Couche `alerts`

| Fichier | Rôle |
|---|---|
| `IAlertHandler.hpp` | Interface : `on_event(const ThermalEvent&)`. |
| `LogAlert.hpp/cpp` | Imprime sur stdout avec `std::println` et `std::format`. |

**Ajout d'un nouveau handler :**
1. Créer une classe qui hérite de `IAlertHandler`.
2. L'enregistrer dans l'`EventBus` au démarrage (`bus.register_handler(...)`).

Exemples possibles : envoi d'email (SMTP), écriture en base de données, activation d'un GPIO, notification MQTT.

---

## 4. Diagramme de classes simplifié

```
┌──────────────────┐        ┌────────────────────────┐
│  ISensorReader   │        │     IAlertHandler      │
│  <<interface>>   │        │     <<interface>>      │
│ + read()         │        │ + on_event(event)      │
│ + sensor_id()    │        └───────────┬────────────┘
└──────┬───────────┘                    │ implements
       │ implements             ┌───────┴──────┐
   ┌───┴──────────┐             │   LogAlert   │
   │ DS18B20Reader│             └──────────────┘
   ├──────────────┤
   │SimulatedSensor│
   └───────────────┘

┌──────────────────────────────────────┐
│          ThresholdMonitor            │
│ - sensor_   : ISensorReader&         │
│ - bus_      : EventBus&              │
│ - config_   : Config                 │
│ - thread_   : std::jthread           │
│ - warn_active_, crit_active_ : bool  │
│ + start() / stop()                   │
└──────────────────────────────────────┘

┌──────────────────────────────────────┐
│              EventBus                │
│ - handlers_ : vector<IAlertHandler> │
│ - mutex_    : std::mutex             │
│ + register_handler(handler)          │
│ + dispatch(ThermalEvent)             │
└──────────────────────────────────────┘
```

---

## 5. Fonctionnalités C++23 utilisées

| Feature | Fichier(s) | Justification |
|---|---|---|
| `std::expected<T,E>` | `ISensorReader.hpp`, `DS18B20Reader.cpp` | Gestion d'erreur sans exception à la frontière matérielle |
| `std::println` / `std::print` | `LogAlert.cpp`, `ThresholdMonitor.cpp` | Remplacement typesafe de `printf` |
| `std::format` | `LogAlert.cpp` | Formatage de chaînes sans flux |
| `std::jthread` + `std::stop_token` | `ThresholdMonitor.cpp` | Thread avec arrêt coopératif intégré |
| Designated initializers | `main.cpp`, tests | Initialisation explicite des structs |
| `std::numbers::pi_v<float>` | `SimulatedSensor.cpp` | Constante mathématique typée |
