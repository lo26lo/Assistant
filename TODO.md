# TODO — MicroscopeIBOM

> Dernière mise à jour : 2026-03-19

---

## 🎨 GUI — Améliorer l'harmonie visuelle

### Layout & Proportions
- [ ] **Dock panels trop serrés** — les margins/spacing sont 4-8px partout, donner plus d'air (12-16px)
- [ ] **BomPanel : colonnes trop serrées** — col "✓" = 30px, "Ref" = 60px → agrandir, utiliser `horizontalHeader()->setStretchLastSection(true)`
- [ ] **StatsPanel : trop dense** — 3 sections horizontales compressées dans max 200px de hauteur → augmenter ou rendre collapsible
- [ ] **ControlPanel : GroupBox mal espacés** — les 4 groupes (Overlay/AI/Camera/Actions) sont empilés sans respiration
- [ ] **Toolbar : icônes 24×24 trop petites** — passer à 32×32, ajouter du padding
- [ ] **StatusBar : 3 labels collés** — ajouter séparateurs visuels ou spacing

### Cohérence visuelle
- [ ] **Fonts incohérentes** — MainWindow utilise 12px, CameraView 15px/11px, StatsPanel default → unifier à une échelle typographique (12/14/18px)
- [ ] **Couleurs de status dispersées** — vert `#00c800`/`#40c040`, rouge `#ff5050`, orange `#ffa500` définis dans 4 fichiers différents → centraliser dans un fichier `Theme.h`
- [ ] **Dark theme hardcodé en inline** — 200+ lignes de CSS dans `MainWindow.cpp` → externaliser dans `resources/styles/dark.qss`
- [ ] **Accent bleu** `#6488e8` → vérifier contraste sur tous les fonds (dock titles `#16161e` vs panel `#1a1a2e`)
- [ ] **GroupBox titres** — styling incohérent entre ControlPanel et StatsPanel
- [ ] **Scrollbar styling** — vérifier que ça marche dans BomPanel (longue liste de composants)

### UX
- [ ] **Placeholder CameraView** — icône caméra géométrique basique → remplacer par une vraie icône SVG
- [ ] **InspectionWizard page Alignment** — juste un QLabel placeholder, pas de contenu réel
- [ ] **Settings dialog** — `onShowSettings()` ne fait rien (juste un log)
- [ ] **Pas de splash screen** — l'app met ~3s au démarrage (GPU warmup), afficher un splash
- [ ] **Menu Help → About** — vérifier que le dialog est complet

---

## 🔌 Wiring — Features non connectées

### Priorité haute (fonctionnalités core)
- [ ] **🔴 Mode Live** — Après calibration, l'overlay suit le PCB en temps réel quand il bouge/tourne sous la caméra. Nécessite :
  - Détection automatique du PCB dans chaque frame (contour board ou feature matching)
  - Recalcul dynamique de l'homographie à chaque frame
  - Support rotation + translation du PCB
  - Interpolation/lissage pour éviter le jitter
  - Toggle ON/OFF dans ControlPanel
- [ ] **InspectionWizard** — signaux non connectés dans `Application.cpp`
- [ ] **Heatmap** — `showHeatmapChanged` signal existe dans ControlPanel mais pas connecté
- [ ] **Manual Homography** — `alignHomographyRequested` signal non connecté

### Priorité moyenne (AI pipeline)
- [ ] **InferenceEngine** — pipeline complète non wired (besoin modèle ONNX)
- [ ] **ComponentDetector** — détection composants AI non connectée
- [ ] **OCREngine** — lecture texte composants non connectée
- [ ] **SolderInspector** — inspection soudure non connectée

### Priorité basse (features avancées)
- [ ] **PickAndPlace** — import fichier pick & place
- [ ] **Measurement** — outil de mesure sur overlay
- [ ] **SnapshotHistory** — historique des captures
- [ ] **BarcodeScanner** — scan code-barre PCB
- [ ] **StencilAlign** — alignement stencil
- [ ] **RemoteView** — visualisation à distance (WebSocket)
- [ ] **VoiceControl** — commandes vocales

---

## 📦 Export
- [ ] **ReportGenerator** — génération rapport PDF/HTML
- [ ] **DataExporter** — export CSV/JSON des résultats

---

## 🐛 Bugs connus
- [ ] **Calibration double path** — log affiche `MicroscopeIBOM/MicroscopeIBOM/calibration.yml` (AppDataLocation inclut org name)
- [ ] **iBOM LZ-String** — décompression corrigée (`numBits=3`), à valider avec fichier réel
- [ ] **Calibration compte** — log dit `1/15` mais code réduit à 5 → vérifier cohérence
- [ ] **parseCommandLine désactivé** — trouver une alternative qui ne crashe pas sur Windows GUI

---

## 🧠 AI / Modèles
- [ ] **Créer/trouver modèle ONNX** pour détection de composants PCB
- [ ] **Dossier `models/`** vide — documenter format attendu
- [ ] **Tester TensorRT EP** — le provider est compilé mais jamais testé avec un vrai modèle
- [ ] **Pipeline inference** : frame → resize → normalize → ONNX → post-process → overlay

---

## 🏗️ Infrastructure
- [ ] **Tests** — `test_ibom_parser` / `test_homography` / `test_inference` / `test_component_matching` compilent mais pas vérifiés
- [ ] **CI/CD** — pas de pipeline (GitHub Actions ou autre)
- [ ] **Installer** — pas de packaging (NSIS, WiX, ou Qt IFW)
- [ ] **README.md** — mettre à jour avec instructions build/run actuelles
