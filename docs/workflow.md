# Workflow — Flux de données et modèle d'exécution

## 1. Démarrage de l'application

```
main()
  │
  ├─ 1. Construit Config (seuils, type de capteur, intervalle)
  │
  ├─ 2. Appelle make_sensor(config)
  │       └─ retourne DS18B20Reader ou SimulatedSensor
  │
  ├─ 3. Crée EventBus
  │
  ├─ 4. Enregistre les handlers (LogAlert, ...)
  │       └─ bus.register_handler(...)
  │
  ├─ 5. Crée ThresholdMonitor(sensor, bus, config)
  │
  ├─ 6. monitor.start()
  │       └─ lance std::jthread → run(stop_token)
  │
  └─ 7. Boucle principale : attend SIGINT / SIGTERM
          └─ monitor.stop() → join du thread
```

---

## 2. Cycle de polling (thread de monitoring)

```
┌─────────────────────────────────────────────────────────┐
│  std::jthread  ::  run(stop_token)                      │
│                                                         │
│  while (!stop_requested)                                │
│    │                                                    │
│    ├─ sensor_.read()                                    │
│    │    ├─ OK  → SensorReading { temp, sensor_id }      │
│    │    └─ Err → log erreur, skip cycle                 │
│    │                                                    │
│    ├─ Évaluation seuil CRITIQUE                         │
│    │    ├─ temp ≥ threshold_crit  &&  !crit_active_     │
│    │    │    → dispatch(ThresholdExceeded, crit)        │
│    │    │    → crit_active_ = true                      │
│    │    └─ temp < threshold_crit − hysteresis           │
│    │         → dispatch(ThresholdRecovered, crit)       │
│    │         → crit_active_ = false                     │
│    │                                                    │
│    ├─ Évaluation seuil WARNING (si !crit_active_)       │
│    │    ├─ temp ≥ threshold_warn  &&  !warn_active_     │
│    │    │    → dispatch(ThresholdExceeded, warn)        │
│    │    │    → warn_active_ = true                      │
│    │    └─ temp < threshold_warn − hysteresis           │
│    │         → dispatch(ThresholdRecovered, warn)       │
│    │         → warn_active_ = false                     │
│    │                                                    │
│    └─ sleep(poll_interval)                              │
└─────────────────────────────────────────────────────────┘
```

---

## 3. Cycle de vie d'un ThermalEvent

```
ThresholdMonitor                EventBus               LogAlert
      │                             │                      │
      │──── dispatch(event) ───────►│                      │
      │                             │──on_event(event) ───►│
      │                             │                      │── std::println(...)
      │                             │◄─────────────────────│
      │◄────────────────────────────│                      │
```

Le dispatch est **synchrone** : `on_event()` est appelé dans le thread de monitoring.
Les handlers doivent donc être non-bloquants ou déléguer à leur propre thread.

---

## 4. Modèle de threading

```
Thread principal                Thread de monitoring
      │                               │
      │  monitor.start()              │
      │ ─────────────────────────►    │  run(stop_token)
      │                               │    └─ boucle polling
      │  (attend signal)              │
      │                               │
      │  g_running = 0 (SIGINT)       │
      │  monitor.stop()               │
      │    └─ request_stop()  ───────►│  stop_requested() = true
      │    └─ join()          ───────►│  (fin propre du thread)
      │◄──────────────────────────────│
      │  return 0
```

**Synchronisation EventBus** : le `std::mutex` de l'EventBus protège l'accès concurrent
à la liste des handlers (enregistrement possible depuis le thread principal pendant que
le monitoring dispatche).

---

## 5. Gestion des erreurs de lecture capteur

```
sensor_.read() retourne std::unexpected(SensorError::...)
         │
         ▼
ThresholdMonitor::run()
  └─ std::println("[ThresholdMonitor] read error: {}", ...)
  └─ skip → sleep → retry au prochain cycle
```

Les erreurs transitoires (bruit sur le bus 1-Wire) sont ignorées silencieusement.
Pour une gestion plus robuste (n erreurs consécutives → événement d'erreur), voir les
évolutions possibles en section 6.

---

## 6. Hystérésis — exemple numérique

```
Config : threshold_warn=50, threshold_crit=65, hysteresis=2

Temps   Temp   warn_active  crit_active  Événement dispatché
  0     45°C       false        false     —
  1     52°C       true         false     ThresholdExceeded (warn, 52°C)
  2     67°C       true         true      ThresholdExceeded (crit, 67°C)
  3     64°C       true         true      — (pas encore sous 65-2=63)
  4     62°C       true         false     ThresholdRecovered (crit, 62°C)
  5     49°C       true         false     — (pas encore sous 50-2=48)
  6     47°C       false        false     ThresholdRecovered (warn, 47°C)
```

---

## 7. Sélection du capteur au runtime

La factory `make_sensor()` dans `main.cpp` instancie le bon type selon `Config::sensor_type` :

```
Config::sensor_type
   │
   ├─ SensorType::Simulated  →  SimulatedSensor("sim-0")
   │                               └─ sinusoïde : base=40°C, amplitude=30°C, période=60s
   │
   └─ SensorType::DS18B20    →  DS18B20Reader(config.device_path)
                                  └─ lit /sys/bus/w1/devices/<id>/temperature
```

Pour changer de capteur : modifier `Config::sensor_type` et, si nécessaire, `Config::device_path`.

---

## 8. Évolutions possibles

| Besoin | Approche recommandée |
|---|---|
| Plusieurs capteurs simultanés | Un `ThresholdMonitor` par capteur, même `EventBus` |
| Alerte email / MQTT | Nouvelle classe `IAlertHandler`, enregistrée dans `main()` |
| Persistance des mesures | Handler dédié qui écrit dans une base SQLite ou InfluxDB |
| N erreurs consécutives → alerte | Compteur dans `ThresholdMonitor::run()`, nouvel enum dans `ThermalEvent::Type` |
| Configuration par fichier | Parser JSON/TOML → remplir `Config` avant de créer le monitor |
| Interface web temps réel | Handler qui publie via WebSocket (ex. Boost.Beast) |
