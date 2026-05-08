# Journal de Session — Migration Jetson AGX Orin

> **But du document** : suivre chronologiquement la progression de la migration vers Jetson, et permettre de reprendre rapidement à toute session.
>
> **Convention** : sessions en ordre **antichronologique** (la plus récente en haut). Le bloc "État actuel" tout en haut est mis à jour à chaque session.
>
> **Documents liés** :
> - [JETSON_MIGRATION.md](JETSON_MIGRATION.md) — plan global + architecture
> - [JETSON_ERREURS.md](JETSON_ERREURS.md) — journal des bugs rencontrés
> - [docker/README.md](../docker/README.md) — quickstart Docker

---

## État actuel — au 2026-05-08

### Phase courante
**Phase 0 — Conteneurisation** : ✅ code livré, ❌ pas encore testé sur le Jetson.

### Branches & tags
| Ref | Pointe sur | Statut |
|-----|------------|--------|
| `main` | `8ae9f2e` (cherry-pick rework README) | actif, dev Jetson |
| `windows-legacy` | `3174dad` (last commit Windows) | gelée, pour repli |
| `v0.1.0-windows-final` (tag) | `3174dad` | archive permanente |

### Matériel
- **Jetson AGX Orin 32GB** (Seeed reComputer J4012) — reçu et opérationnel
- **JetPack 6.2** (L4T R36.4, Ubuntu 22.04) — installé d'origine Seeed
- **Écran tactile Minix SF16T** — prévu (HID-multitouch USB-C standard)
- **Caméra microscope USB** — UVC standard (V4L2)
- **RealSense D405** — prévue (futur, librealsense2 déjà packagée)

### Ce qui est livré
- Plan de migration complet : [docs/JETSON_MIGRATION.md](JETSON_MIGRATION.md)
- Stack Docker complet dans [docker/](../docker/)
- Script de build Jetson : [scripts/build_jetson.sh](../scripts/build_jetson.sh)
- Configuration cross-platform .gitattributes + .dockerignore
- Journaux de session + erreurs : [JETSON_SESSION_LOG.md](JETSON_SESSION_LOG.md), [JETSON_ERREURS.md](JETSON_ERREURS.md)
- Règles strictes de tenue de journaux dans [CLAUDE.md](../CLAUDE.md) (mise à jour avant chaque push obligatoire)

### Ce qui reste à faire
- [ ] **Tester `docker compose build base`** sur le Jetson (~90-120 min première fois)
- [ ] **Tester `docker compose build dev`** + lancer le container interactif
- [ ] **Tester le build C++** dans le container : `bash scripts/build_jetson.sh`
- [ ] Phase 1 : nettoyer les `#ifdef IBOM_PLATFORM_WINDOWS` au cas par cas (uniquement si bloque le build)
- [ ] Phase 2 : refactor `FrameBuffer` pour mémoire unifiée (zero-copy)
- [ ] Phase 3 (si nécessaire) : DLA + INT8 pour le ComponentDetector

### Blocages connus
Aucun pour l'instant — en attente du premier test sur Jetson.

### Comment reprendre à la prochaine session
1. Lire ce bloc "État actuel" + la dernière entrée de session ci-dessous
2. Vérifier le statut de [JETSON_ERREURS.md](JETSON_ERREURS.md) pour les bugs ouverts
3. Sur le Jetson : `cd ~/Assistant-git && git pull && git status`
4. Continuer là où la dernière session s'est arrêtée

---

## Session 2026-05-08 — Démarrage migration

### Objectif
Concevoir et lancer la migration de MicroscopeIBOM (projet Windows fonctionnel) vers Jetson AGX Orin 32GB en environnement Docker, sans casser la version Windows.

### Contexte de départ
- Projet C++20 / Qt6 / OpenCV / ONNX Runtime / TensorRT, fonctionnel sur Windows MSVC + vcpkg
- 5 commits Windows récents (camera enumeration thread, mesures, panel inspection, tracking Lowe ratio, doc)
- Décision utilisateur : porter sur Jetson AGX Orin 32GB Seeed reComputer (JP 6.2)

### Décisions prises
| Sujet | Décision | Pourquoi |
|-------|----------|----------|
| Plateforme | Jetson AGX Orin 32GB (Seeed J4012) | Matériel déjà disponible |
| OS | JetPack 6.2 → 7.x quand Seeed compatible | Stabilité actuelle, migration future facile |
| Conteneurisation | Docker via [dusty-nv/jetson-containers](https://github.com/dusty-nv/jetson-containers) | Ne pas polluer L4T, migration JP6→JP7 indolore |
| Niveau de refactor | Medium (phases 0-2 obligatoires, phase 3 optionnelle) | "Ne rien perdre" en perfs |
| Modèle YOLO cible | YOLOv8m FP16 (à confirmer) | Bon compromis vitesse/précision PCB |
| Stratégie d'archivage | Tag + branche `windows-legacy` sur dernier commit Windows | Permettre repli sans pollution |
| Démarrage app | Lancement manuel `docker compose up runtime` | Pas d'autostart systemd pour l'instant |
| Fichiers Windows | Garder en place pendant Phase 1 | Référence pratique, suppression progressive |

### Ce qui a été fait

#### Analyse et planification
- Audit complet du code source pour évaluer la portabilité
- Identification des `#ifdef IBOM_PLATFORM_WINDOWS` (déjà avec branches Linux préexistantes)
- Évaluation des dépendances vcpkg vs apt vs build-from-source
- Analyse du stack JetPack 6.2 (CUDA 12.6, TRT 10.3, cuDNN 8.9)

#### Documents créés
- [docs/JETSON_MIGRATION.md](JETSON_MIGRATION.md) — plan complet (12 sections, ~700 lignes) couvrant :
  contraintes utilisateur, architecture Docker, config matérielle, choix YOLO, plan en phases, layout repo, squelettes Docker, migration JP6→JP7, checklist mise en route, troubleshooting

#### Archivage Windows
- Tag `v0.1.0-windows-final` créé sur `3174dad` (dernier commit Windows réel)
- Branche `windows-legacy` créée sur `3174dad`
- Tag + branche pushés sur origin
- ⚠️ Initialement créés sur `250e327` puis re-pointés sur `3174dad` après rebase pour inclure 5 commits Windows manquants

#### Phase 0 — Docker (livrée)
Commit `40be3fd feat(docker): Phase 0 conteneurisation Jetson AGX Orin`
- [docker/base.Dockerfile](../docker/base.Dockerfile) : multi-stage avec OpenCV CUDA + librealsense2 compilés from source, Qt6 (apt), ZXing-cpp (from source), spdlog/json/libharu (apt)
- [docker/dev.Dockerfile](../docker/dev.Dockerfile) : + gdb, valgrind, vcpkg, ccache, clang-format
- [docker/runtime.Dockerfile](../docker/runtime.Dockerfile) : minimale, binaire stripped + entrypoint
- [docker/compose.yml](../docker/compose.yml) : services base/dev/runtime avec runtime nvidia, network_mode host, devices /dev/video* + /dev/bus/usb + /dev/input + /dev/dri, X11 forwarding
- [docker/run-dev.sh](../docker/run-dev.sh) : wrapper xhost/xauth/lancement interactif
- [docker/entrypoint.sh](../docker/entrypoint.sh) : sanity checks runtime + génération engines TRT au premier lancement
- [docker/README.md](../docker/README.md) : quickstart + troubleshooting
- [scripts/build_jetson.sh](../scripts/build_jetson.sh) : build CMake/Ninja dans le container, ASAN optionnel
- `.gitattributes` : force LF sur scripts (anti-CRLF Windows)
- `.dockerignore` : exclut build/, models/, .git/...

#### Nettoyage repo
- Branche `claude/rework-readme-0TDMk` mergée via cherry-pick (commit `8ae9f2e`)
- Branche remote supprimée
- Pruned les refs locales

#### Journaux créés (cette session)
- [docs/JETSON_SESSION_LOG.md](JETSON_SESSION_LOG.md) (ce fichier)
- [docs/JETSON_ERREURS.md](JETSON_ERREURS.md)

### Choix techniques notables
- **OpenCV CUDA compilé from source** dans le base.Dockerfile (~90 min) plutôt que copier depuis `dustynv/opencv` → image autonome, indépendante de tags qui peuvent disparaître
- **librealsense2 from source** car pas de paquet apt ARM64 officiel
- **CUDA_ARCH_BIN=8.7** (Ampere — architecture du Jetson AGX Orin)
- **Qt 6.2 LTS via apt** (Ubuntu 22.04) — suffisant pour le projet, upgrade vers 6.6+ uniquement si besoin remonte
- **Multi-stage Docker** : 3 stages (opencv-builder, realsense-builder, final) — réduit la taille finale et permet caching

### À faire prochaine session
1. **Sur le Jetson** :
   ```bash
   git pull
   sudo nvpmodel -m 0 && sudo jetson_clocks
   docker compose -f docker/compose.yml build base   # 90-120 min
   ```
2. Reporter les erreurs éventuelles dans [JETSON_ERREURS.md](JETSON_ERREURS.md)
3. Si `base` build OK → `docker compose build dev` puis `bash docker/run-dev.sh`
4. Dans le container : `bash scripts/build_jetson.sh` (premier build C++ sur Jetson)
5. **Probablement des erreurs de compilation** : c'est le déclencheur de la Phase 1 (nettoyage `#ifdef`)

### Commits poussés cette session
| Hash | Message |
|------|---------|
| `93765fa` | docs: add Jetson AGX Orin migration plan |
| `40be3fd` | feat(docker): Phase 0 conteneurisation Jetson AGX Orin |
| `8ae9f2e` | docs: rework README — structure, précision, lisibilité (cherry-pick) |
| (commit suivant) | docs: add Jetson session and error logs |
| (commit suivant) | docs(claude): mandatory session log discipline |
| `v0.1.0-windows-final` (tag) | Archive Windows |
| `windows-legacy` (branche) | Archive Windows |

### Notes / observations
- Le repo contient encore `build_windows.bat` et `scripts/install_prerequisites.bat` — c'est volontaire (référence pendant Phase 1, à supprimer plus tard).
- Les `#ifdef IBOM_PLATFORM_WINDOWS` dans le code sont laissés en place — la branche `#else` Linux existe déjà donc le code devrait compiler tel quel sur Jetson.
- L'utilisateur travaille **depuis Windows** (cwd `c:\Users\bambo\Desktop\Assistant\Assistant-git`) — les fichiers sont créés ici puis poussés sur GitHub, le Jetson fait `git pull`.

---

<!-- AJOUTER LES NOUVELLES SESSIONS AU-DESSUS DE CETTE LIGNE -->

## Modèle pour nouvelle session

```markdown
## Session YYYY-MM-DD — Titre court

### Objectif
...

### Contexte de départ
...

### Ce qui a été fait
...

### Décisions prises
...

### Erreurs rencontrées
Voir [JETSON_ERREURS.md](JETSON_ERREURS.md) entrées #N à #M

### À faire prochaine session
1. ...

### Commits poussés
| Hash | Message |
|------|---------|

### Notes / observations
...
```
