# Recherche approfondie de bugs — 2026-07-18

> **Mise à jour 2026-07-18 (même session)** : les 11 bugs B1-B11 sont **corrigés** (commit sur cette
> branche), plus **2 bugs bonus découverts pendant la correction** : **B12** (les couleurs d'état du
> BomPanel ne s'appliquaient jamais — `state == "placed"` comparé à la valeur réelle `"Placed"`) et
> **B13** (`m_ibomHash` calculé **avant** `setIBomFilePath()` → le journal d'audit et la clé de scan
> étaient estampillés avec le hash de la carte *précédente* après un changement d'iBOM). Détail des
> correctifs en fin de document (§ « Corrections appliquées »). ⚠️ Rien n'a pu être compilé dans ce
> conteneur (pas de Qt/OpenCV) — build + ctest à valider au prochain build Jetson.

> **Méthode** : revue statique ciblée de ~12 000 lignes, priorisée sur le code jamais exécuté sur
> Jetson (suites 145-149 : minimap V2, scan mosaïque, golden diff, depth check, reconnexion caméra,
> RemoteView token, tour guidé, loupe, gates mm, Auto 1-fit) + les chemins critiques de threading
> (Application, CameraCapture, TrackingWorker). Chaque bug listé a été **vérifié dans le code**
> (fichier:ligne) — pas de spéculation. Aucun code n'a été modifié : ce document est le livrable.
>
> Fichiers audités en entier : `Application.{h,cpp}`, `CameraCapture.cpp`, `TrackingWorker.cpp`,
> `RemoteView.cpp`, `BoardScanner.{h,cpp}`, `BoardMosaic.cpp`, `GoldenDiff.cpp`, `DepthInspector.cpp`,
> `SceneQuality.cpp`, `PickAndPlace.cpp`, `ProjectDiff.cpp`, `BoardMinimap.cpp`, `CameraView.cpp`,
> `HeatmapRenderer.cpp`, `Homography.cpp`, `ImageUtils.cpp/h`, `main.cpp` (+ extraits ControlPanel,
> BomPanel, MainWindow).

---

## Index par sévérité

| # | Sévérité | Composant | Titre court |
|---|----------|-----------|-------------|
| B1 | 🟠 Moyen | PickAndPlace | Tri « Position » sans effet — `order` écrasé par le tri par valeur au chargement |
| B2 | 🟠 Moyen | Application / BomPanel / Minimap | Reset d'inspection : minimap et états BOM jamais nettoyés |
| B3 | 🟠 Moyen | Application (golden A2) | Cache scan/golden non invalidé au chargement d'un nouvel iBOM → golden de la carte A sauvé sous le hash de la carte B |
| B4 | 🟠 Moyen | Application / HeatmapRenderer | Heatmap de défauts non purgée au changement de carte |
| B5 | 🟡 Moyen-faible | Application / ControlPanel | Combo device : `setCurrentIndex(index /dev/video)` — confusion position/données (ERREUR #22 ressuscitée) + `findChild<QComboBox*>()` fragile |
| B6 | 🟡 Faible-moyen | Application (depth) | `m_lastDepthFrame` jamais invalidé au switch de backend → Depth-Check/Auto-Align sur profondeur périmée |
| B7 | 🟡 Faible | TrackingWorker | CLAHE appliqué in-place sur le buffer partagé zero-copy (caméra monochrome) |
| B8 | 🟡 Faible | Application (minimap anchor) | Ancre minimap microscope : miroir face arrière ignoré + pas de garde `scale<=0` |
| B9 | 🟡 Faible | Application (teardown) | Fermeture pendant Auto-Align/re-anchor : la lambda QtConcurrent peut survivre aux membres détruits |
| B10 | 🟡 Faible | PickAndPlace / InspectionPanel | Fin de tour avec steps sautés : cul-de-sac silencieux |
| B11 | 🟡 Faible | Application | Restauration de session échouée : lignes BOM restent marquées « Placed » |
| B12 | 🟡 Faible (bonus) | BomPanel | Couleurs d'état jamais appliquées — `"placed"` comparé à `"Placed"` |
| B13 | 🟡 Faible (bonus) | Application | `m_ibomHash` calculé avant `setIBomFilePath()` → journal d'audit estampillé avec le hash de la carte précédente |

**Tous corrigés (B1-B13)** — voir § « Corrections appliquées » en fin de document.
Sections « Notes & risques mineurs » et « Chemins vérifiés sains » en fin de document.

---

## B1 — 🟠 Tri « Position » sans effet (PickAndPlace)

**Où** : `src/features/PickAndPlace.cpp`

- `loadComponents()` (l.15-40) assigne `step.order = 0..n` dans l'ordre de chargement iBOM, **puis
  appelle `sortByValueGroup()`** (l.34) qui **renumérote** `order = i` selon le tri par valeur
  (l.50-52).
- `sortByPosition()` (l.74-81) trie ensuite « par `order` » en croyant retrouver l'ordre spatial de
  chargement : *« original load order corresponds to spatial layout »*. Mais à ce stade `order` est
  déjà l'ordre value-group → **`SortMethod::Position` est un no-op** qui rend exactement le tri par
  valeur.

**Impact** : le réglage Settings → Component order → « Position » ment à l'utilisateur.

**Fix suggéré** : soit trier réellement par `position` (x+y ou tri lexicographique y puis x), soit
mémoriser l'ordre de chargement dans un champ séparé (`loadOrder`) jamais renuméroté. À noter :
`sortByNearestNeighbor()` (suite 149) couvre déjà le besoin « trajet spatial » — « Position »
pourrait aussi simplement être documenté/retiré.

---

## B2 — 🟠 Reset d'inspection : minimap et états BOM jamais nettoyés

**Où** : `src/app/Application.cpp` l.3784-3791 (handler `resetClicked`)

```cpp
m_placedRefs.clear();
m_placedOrder.clear();
appendInspectionLog("reset", "*");
refreshInspectionStats();
saveInspectionState();
```

Le handler ne rappelle **ni** `boardMinimap()->setPlacedRefs(m_placedRefs)` **ni** un nettoyage des
états de lignes BomPanel (`setComponentState(ref, QString())`). Vérifié côté BomPanel :
`setProgress()` ne touche qu'un label (`BomPanel.cpp` l.139-142), et la seconde connexion
`resetClicked → PickAndPlace::reset` (l.3818) ne nettoie que l'état interne de PickAndPlace.

**Impact** : après un Reset, la PCB Map continue d'afficher les points verts « placés » et le tableau
BOM garde « Placed »/« Skipped » sur toutes les lignes, alors que la progression affiche 0/N.
L'overlay caméra, lui, se corrige (le hash `m_ovSigPlacedHash` change) → incohérence visible entre
les trois surfaces.

**Fix suggéré** : dans le handler reset, itérer `m_placedRefs` *avant* le clear pour blanchir les
lignes BOM (ou exposer un `BomPanel::clearAllStates()`), puis pousser le set vide à la minimap.

---

## B3 — 🟠 Cache scan/golden non invalidé au chargement d'un nouvel iBOM

**Où** : `src/app/Application.cpp` — `loadIBomFile()` (l.4103-4239) vs `saveGolden()` (l.4921) /
`compareGolden()` (l.4963)

`m_lastScanMosaic`, `m_lastScanMask`, `m_lastScanGeo`, `m_lastScanLayer` sont remplis par
`onScanFinished()` et **jamais invalidés** quand un autre iBOM est chargé. Or `saveGolden()` et
`compareGolden()` clef-ent le store par `ibomContentHash()` — le hash du fichier **courant**.

**Scénario de corruption** :
1. Scanner la carte A (mosaïque en cache).
2. Charger l'iBOM de la carte B.
3. « Save Last Scan as Golden » → la mosaïque de **A** est écrite sous
   `golden/<hash de B>/front.png`. Le store golden de B est silencieusement corrompu ; tous les
   compares futurs de B flagueront des dizaines de faux défauts.
4. Symétriquement, « Compare Last Scan to Golden » compare le scan de A au golden de B, avec les
   composants de B (`scoreComponents(..., m_ibomProject->components, ...)`) — résultats absurdes
   sans message d'erreur.

De plus, un **scan actif** n'est pas arrêté par `loadIBomFile()` : `m_scanActive` reste vrai, le
worker continue d'accumuler sur la géométrie de l'ancienne carte pendant que la pose passe à la
nouvelle (l'auto-homographie bbox l.4196-4224 rend la pose « valide » → le gate `frameReady`
continue de nourrir le scanner).

**Fix suggéré** : au début de `loadIBomFile()` (après parse réussi) : si `m_scanActive`, appeler
`onBoardScanToggled(false)` ; puis `m_lastScanMosaic.release(); m_lastScanMask.release();
m_lastScanGeo = {};`.

---

## B4 — 🟠 Heatmap de défauts non purgée au changement de carte

**Où** : `src/app/Application.cpp` — `compareGolden()` l.5019-5033 vs `loadIBomFile()`

`m_heatmapRenderer` est rempli par `compareGolden()` (coordonnées **relatives au bbox de la carte
comparée** : `a.pcbCenter.x - bb.minX`). Ni le renderer ni `m_showHeatmap` ne sont réinitialisés
dans `loadIBomFile()`. Le cache overlay est bien re-rendu (le pointeur projet change la signature),
mais le rendu composite (frameReady l.2595-2606) dessine `renderArgb()` — le grid de défauts de
**l'ancienne carte** — étiré sur `QRectF(bb.minX, bb.minY, bb.width(), bb.height())` de la
**nouvelle** carte.

**Impact** : avec « Show Defect Heatmap » coché, charger une autre carte affiche des taches de
défauts fantômes à des positions sans signification.

**Fix suggéré** : dans `loadIBomFile()` : `m_heatmapRenderer->clear(); ++m_heatmapRev;`
(le `++m_heatmapRev` force la re-signature). Optionnel : décocher `m_showHeatmap` + le toggle UI.

---

## B5 — 🟡 Combo device sélectionné par position avec l'index /dev/video réel

**Où** : `src/app/Application.cpp` l.3116-3118 (handler `settingsChanged`)

```cpp
auto* cp = m_mainWindow->controlPanel();
if (cp && newIdx >= 0)
    cp->findChild<QComboBox*>()->setCurrentIndex(newIdx);
```

Deux problèmes vérifiés :
1. `findChild<QComboBox*>()` renvoie le **premier** QComboBox du ControlPanel, sans garantie que ce
   soit le combo device (et un retour nul déréférencé planterait — improbable mais non gardé).
2. `newIdx` est l'index **/dev/video réel** (données d'item), passé comme **position** de combo.
   C'est exactement la confusion documentée en ERREUR #22. Avec le microscope sur `/dev/video6` et
   un combo de 2 entrées, `setCurrentIndex(6)` → sélection -1 → combo vide après un « Apply » des
   Settings.

Or `ControlPanel::setCameraDevices()` (l.321-335) fait déjà la re-sélection correcte par
`findData(currentIndex)`.

**Fix suggéré** : supprimer ces 3 lignes et appeler `refreshCameraDeviceList()` à la place (ou
exposer un setter dédié qui fait `findData`).

---

## B6 — 🟡 `m_lastDepthFrame` jamais invalidé au switch de backend

**Où** : `src/app/Application.h` l.355 ; `switchCameraBackend()` l.852-887 ; consommateurs :
`runDepthInspection()` l.5065, `autoAlignBoard()` l.1358.

Le cache profondeur n'est rempli que par le handler RealSense, mais il **survit** au passage au
backend V4L2 (rien ne le nettoie ; `updateCalibrationUI()` ne blanchit que l'affichage Distance /
Depth fill — même famille que l'ERREUR #25 déjà corrigée pour les *stats*).

**Impact** : sur le profil microscope, « Depth-Check Components (D405) » s'exécute sur la dernière
frame D405 d'une session précédente, croisée avec l'homographie **microscope** courante → verdicts
ABSENT/present aléatoires présentés sans avertissement. `autoAlignBoard()` passe aussi cette
`depthCopy` périmée à `BoardLocator::locate()`.

**Fix suggéré** : `m_lastDepthFrame.reset(); m_lastDepthDistanceMm = 0.0;` dans
`switchCameraBackend()` (et, par sûreté, gater `runDepthInspection()` sur
`m_activeBackend == CameraBackend::RealSense`).

---

## B7 — 🟡 TrackingWorker : CLAHE in-place sur le buffer partagé zero-copy

**Où** : `src/overlay/TrackingWorker.cpp` l.796-812 (`processFrame`)

```cpp
cv::Mat gray;
if (frame->channels() == 3)
    cv::cvtColor(*frame, gray, cv::COLOR_BGR2GRAY);   // OK : nouveau buffer
else
    gray = *frame;                                    // copie superficielle du buffer PARTAGÉ
...
if (m_useClahe && m_clahe) m_clahe->apply(gray, gray); // écrit DANS le buffer partagé
```

Pour une caméra qui livre du **monochrome natif** (CV_8UC1 — supporté par le chemin d'affichage
`wrapMatShared`/`Format_Grayscale8`), `gray` partage les pixels de la `FrameRef` immuable :
`CLAHE::apply(src, dst)` avec `dst == src` réutilise le buffer existant (`create()` no-op) →
**écriture dans la frame que le GUI est en train d'afficher** (data race + image égalisée visible
localement + fausse le contrat « const » du zero-copy, cf. règle n°14 du CLAUDE.md).

Non déclenchable avec les caméras actuelles (BGR 3 canaux), mais c'est une mine posée sur un
invariant central.

**Fix suggéré** : `else gray = frame->clone();` quand `m_useClahe`, ou
`m_clahe->apply(gray, tmp); gray = tmp;`.

---

## B8 — 🟡 Ancre minimap (microscope) : miroir face arrière ignoré

**Où** : `src/app/Application.cpp` l.3938-3976 (handler `anchorRequested`, chemin microscope)

Tous les autres chemins de similarité (2-comp l.3533, anchor-clic l.3464, multi-align n=2 l.1270)
appliquent `vx = -1` sur la face Back pour encoder le miroir dans l'homographie. Le handler
minimap construit la similarité **sans** `vx` : sur la face arrière, un clic minimap au microscope
pose un overlay non miroité (composants inversés gauche-droite).

Secondaire, même handler : pas de garde `scale <= 0` (le chemin clic a `if (scale <= 0) scale=20`,
l.3457) — si `m_currentPixelsPerMm == 0` **et** `microscope.anchor_pixels_per_mm` mis à 0 par
l'utilisateur, `cosR = sinR = 0` → `compute()` échoue silencieusement (les 4 coins projetés sont
confondus).

**Fix suggéré** : factoriser la construction de similarité (un helper `similarityHomography(scale,
rotDeg, vx, pivot…)`) utilisée par les 4 chemins ; ajouter le fallback 20 px/mm.

---

## B9 — 🟡 Fermeture pendant Auto-Align / re-anchor : lambda QtConcurrent orpheline

**Où** : `src/app/Application.cpp` — `autoAlignBoard()` l.1555, `componentReanchor()` l.1915 ;
`~Application()` l.254-281.

Les deux dispatchent sur `QtConcurrent::run` avec un `QFutureWatcher` parenté à `Application`, mais
le **destructeur n'attend jamais les futures**. À la fermeture pendant une détection en vol :
- le watcher est détruit (le callback ne tirera pas — OK),
- la lambda du pool **continue** et utilise `detector` (pointeur brut vers
  `m_componentDetector`, détruit par le dtor) et indirectement `m_inferenceEngine` → use-after-free
  possible ; même sans détecteur, `detectComponentBlobs`/`BoardLocator` tournent pendant que spdlog
  est encore vivant (OK) mais le process peut sortir de `main()` avant la fin de la tâche —
  le drain du pool global se fait en destruction statique, après la destruction de tout `ibom::*`.

Fenêtre étroite (il faut quitter pendant les ~0,1-4 s d'une détection), mais c'est le même genre de
course que l'ERREUR #43 — et elle se manifesterait pareil : segfault après « Application exiting ».

**Fix suggéré** : conserver les watchers actifs (membre `std::vector<QFutureWatcher*>` ou un simple
`QFuture` membre par flux) et faire `future.waitForFinished()` en tête de `~Application()`, avant la
destruction des sous-systèmes.

---

## B10 — 🟡 Fin de tour avec steps sautés : cul-de-sac silencieux

**Où** : `src/features/PickAndPlace.cpp` — `markPlaced()` l.165-182, `skip()` l.184-190

Quand le **dernier** step est placé alors que des steps antérieurs ont été sautés :
`m_currentIndex` passe à `size`, `isComplete()` est faux (les sautés ne sont pas placés), donc ni
`allPlaced` ni `currentStepChanged` ne sont émis → l'InspectionPanel reste figé sur le dernier
composant, `currentStep()` renvoie le step vide statique, **P** ne fait plus rien
(`markPlaced` early-return), **N** ne fait plus rien (`skip` exige `index < size-1`). Seul
**Shift+N** (goBack) sort de l'impasse, sans aucun message.

**Fix suggéré** : dans `markPlaced()`/`skip()`, quand l'index atteint la fin avec des steps non
placés restants, boucler sur le premier step non placé (comportement « wrap ») ou émettre un signal
`remainingSkipped(count)` que l'Application transforme en message de statut.

---

## B11 — 🟡 Restauration de session échouée : lignes BOM restent « Placed »

**Où** : `src/app/Application.cpp` — `loadIBomFile()` l.4157-4165 vs `startInspection()` l.4267-4278

Au chargement, les refs sauvegardées sont appliquées au BomPanel (« Placed » ligne par ligne).
Dans `startInspection()`, si `restorePlaced()` ne matche rien (`restored == 0` — ex. session_state
d'une autre révision du même chemin de fichier), on fait `m_placedRefs.clear()` mais les lignes
BomPanel marquées au load **restent** « Placed » et la minimap garde ses points (même famille que
B2 — le nettoyage des surfaces n'est jamais fait nulle part).

**Fix suggéré** : même helper de nettoyage que B2, appelé quand la restauration est abandonnée.

---

## Notes & risques mineurs (pas des bugs francs, à connaître)

1. **BoardMosaic — snap de ROI négatif** (`BoardMosaic.cpp` l.101-106) : `(roi.x / m_tilePx) *
   m_tilePx` tronque vers zéro pour un `roi.x` négatif (snap vers l'*intérieur*), et `roi.width` est
   recalculé à partir du `br()` déjà décalé. Sans conséquence car tout est ensuite clampé au canvas,
   mais quelques pixels de bord peuvent être perdus sur les tuiles extrêmes.
2. **SceneQuality — netteté hors ROI** (`SceneQuality.cpp` l.81-87) : la variance Laplacienne est
   calculée sur **toute** la frame, pas sur la ROI carte (contrairement à l'exposition/glare). Un
   fond très texturé (bois) peut masquer un flou de la carte → le conseiller « out of focus » sous-
   détecte.
3. **RemoteView — comparaison de token non constant-time** et token 10 chars alphanum : correct pour
   un LAN d'atelier, à durcir si un jour exposé au-delà.
4. **`saveInspectionState()` non atomique** (écriture directe de session_state.json) : un kill
   pendant l'écriture peut tronquer le JSON — le parse échouera silencieusement (perte de la
   progression, pas de crash). Écrire vers un `.tmp` + rename si on veut la robustesse.
5. **`Homography::setMatrix()`** n'exige pas l'inversibilité : une matrice singulière donnerait un
   `m_inverse` invalide utilisé par `imageToPcb()`. Tous les appelants actuels passent des matrices
   issues de fits valides, mais la classe ne se défend pas.
6. **`sortByNearestNeighbor()` ignore la face** : la route mélange composants Front et Back — le
   trajet « le plus court » traverse la carte pour des composants qu'on ne voit pas. Cohérent avec
   le comportement des autres tris (aucun ne filtre), mais surprenant en tour guidé.
7. **`attachPeer()` — partage copy-on-write de `m_coverage`** (`BoardMinimap.cpp` l.168) : dès la
   première écriture, dock et grande vue divergent et accumulent chacune leur raster (les deux
   reçoivent `accumulateCoverage()`, donc pas de désync visible — juste un double travail).
8. **`markPlaced` sur inspection jamais démarrée** : `requireTour()` protège P/N, mais le bouton
   « Placed » du panel appelle `markPlaced` directement — sans étapes chargées, no-op propre. OK.
9. **Crash handler** (`main.cpp` l.12-25) : `strsignal`/spdlog ne sont pas async-signal-safe —
   choix assumé (déjà documenté ERREUR #43), à garder en tête si le handler re-crash un jour.
10. **Double-clic CameraView pendant un picking d'alignement** : le press gauche ajoute un point
    *puis* le double-clic bascule le fullscreen — on peut cliquer un coin et se retrouver en plein
    écran. Préexistant, faible.

## Chemins vérifiés sains (pour mémoire)

- **CameraCapture reconnexion à chaud (suite 146)** : backoff, adoption d'un nouveau /dev/videoN,
  `stop()` réactif (sommeil tranché 100 ms), pas de fuite de thread (join systématique dans
  `start()`/`stop()`), émission `captureStateChanged` cohérente — RAS.
- **TrackingWorker `reuseAutoChoice` (§1.1)** : fallback arbitration complète quand la famille
  mémorisée échoue, reset de `m_autoChoice` dans `resetReference()` — RAS.
- **RemoteView token (§3.2)** : gate binaire/status sur `isAuthed`, `GET_HTML` ne bake le token que
  pour un client authentifié, la copie disque locale (paramètre défaut `includeToken=true`) est
  intentionnelle — RAS.
- **ImageUtils zero-copy (`wrapMatOwned`/`wrapMatShared`)** : cleanup hooks corrects, stride en
  `qsizetype`, fallback conversion pour les types exotiques — RAS.
- **Gates re-anchor en mm, ReanchorGate, scan gating, scene advisor streaks, GoldenDiff /
  DepthInspector / ProjectDiff (purs, unit-testés)** : relus, RAS.
- **Signatures signaux/slots cross-thread** (BoardScanner, DatasetCreator, TrackingWorker) :
  métatypes enregistrés, slots publics, arguments par valeur — RAS.

---

## Corrections appliquées (2026-07-18, même session)

| Bug | Correctif |
|-----|-----------|
| B1 | `sortByPosition()` trie désormais réellement en ordre raster (y puis x) sur `position`, et renumérote `order` (l'ancien « tri par load order » était détruit par `sortByValueGroup()` au chargement). |
| B2 | Nouveau `BomPanel::clearAllStates()` ; le handler Reset blanchit toutes les lignes BOM **et** pousse le set vide à la minimap (`setPlacedRefs`). |
| B3 | `loadIBomFile()` stoppe un scan en vol et purge `m_lastScanMosaic/Mask/Geo` ; garde d'identité `m_scanIbomHash` : `onScanFinished()` rejette un résultat dont la carte n'est plus chargée (le `scanFinished` du worker arrive en différé après la purge). |
| B4 | `loadIBomFile()` purge le `HeatmapRenderer` (+ `++m_heatmapRev` pour re-signer le cache overlay) et efface `m_selectedRef`. |
| B5 | Les 3 lignes `findChild<QComboBox*>()->setCurrentIndex(newIdx)` remplacées par `refreshCameraDeviceList()` (re-sélection par `findData`, jamais par position). |
| B6 | `switchCameraBackend()` invalide `m_lastDepthFrame` + `m_lastDepthDistanceMm` ; `runDepthInspection()` gate explicitement sur le backend RealSense. |
| B7 | `TrackingWorker::processFrame()` : frame mono + CLAHE → `frame->clone()` avant l'apply in-place (le buffer FrameRef partagé n'est plus jamais écrit). |
| B8 | Handler `anchorRequested` minimap : facteur miroir `vx` face Back (même convention que les autres chemins de similarité) + fallback `scale <= 0 → 20 px/mm`. |
| B9 | Les `QFuture` d'Auto-Align / re-anchor sont conservés en membres et attendus (`isStarted()` + `waitForFinished()`) en tête de `~Application()`, avant la destruction du détecteur. |
| B10 | `markPlaced()` et `skip()` bouclent sur le premier step non placé quand la fin de liste est atteinte avec des steps sautés restants (plus de cul-de-sac). |
| B11 | `startInspection()` : restauration abandonnée → `clearAllStates()` + minimap vidée. |
| B12 | `setComponentState()` : comparaison d'état **insensible à la casse** (`"Placed"` colorie enfin en vert). Limite connue : si des traductions sont un jour livrées, `tr("Placed")` ne matchera plus — passer alors par un enum. |
| B13 | `m_ibomHash = ibomContentHash()` déplacé **après** `m_config->setIBomFilePath(path)` dans `loadIBomFile()`. |

**Fichiers modifiés** : `src/app/Application.{h,cpp}`, `src/features/PickAndPlace.cpp`,
`src/gui/BomPanel.{h,cpp}`, `src/overlay/TrackingWorker.cpp`.

**Non compilé ici** (pas de Qt/OpenCV dans le conteneur). Points d'attention au prochain build :
1. `Application.h` inclut désormais `overlay/BoardLocator.h` + `overlay/ComponentReanchor.h`
   (membres `QFuture<T>`) — ce qui tire `<onnxruntime_cxx_api.h>` dans `main.cpp` ; sans risque car
   `onnxruntime::onnxruntime` est linké sur toute la cible (includes propagés), et la CI fournit le
   stub ONNX aux passes `-fsyntax-only`.
2. `ctest` : `test_pickandplace` ne couvrait ni `sortByPosition` ni skip-en-fin-de-liste — les
   asserts existants (route NN, unplace) restent valides ; ajouter idéalement un test raster + un
   test wrap B10.
3. Scénario terrain B3 : scanner la carte A, charger la carte B pendant le scan → statut
   « Board scan discarded — a different iBOM was loaded during the scan », et `golden/<hashB>/`
   ne doit **pas** être créé par « Save Last Scan as Golden » (message « No finished board scan »).
