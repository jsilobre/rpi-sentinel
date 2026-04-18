# Documentation — rpi-temp-monitor

Référence technique pour les développements présents et futurs.

| Document | Contenu |
|---|---|
| [architecture.md](architecture.md) | Composants, couches, interfaces, dépendances |
| [workflow.md](workflow.md) | Flux de données, cycle de vie des événements, modèle de threading |
| [build-guide.md](build-guide.md) | Compilation, tests, CI/CD, portage RPi |

## Vue d'ensemble rapide

```
Capteur physique / simulé
        │
        ▼
  ThresholdMonitor  ──(std::jthread)──►  lecture périodique
        │
        ▼  (seuil franchi)
    EventBus  ──►  LogAlert / futurs handlers
```

Le projet est structuré en **4 couches indépendantes** (`sensors`, `events`, `monitoring`, `alerts`)
reliées par des interfaces abstraites, ce qui permet d'ajouter un nouveau capteur ou un nouveau
type d'alerte sans toucher au reste du code.
