# Propositions de nouvelles features — 2026-07-12

> **Demande** : « propose-moi de nouvelles features dans un plan structuré ».
> **Méthode** : exclusion volontaire de tout ce qui est déjà tracé ailleurs — la matrice §11 de
> [INVESTIGATION_360_2026-07.md](INVESTIGATION_360_2026-07.md), les items restants de
> [IDEES_AMELIORATIONS.md](IDEES_AMELIORATIONS.md) (BarcodeScanner, i18n, InferenceWorker…) et les
> plans dataset/IA ([DATASET_CREATOR_PLAN.md](DATASET_CREATOR_PLAN.md), [AI_PIPELINE.md](AI_PIPELINE.md)).
> Tout ce qui suit est **nouveau**, et s'appuie délibérément sur les briques livrées récemment :
> homographie continue fiable (suites 126-142), couverture d'inspection minimap (suite 145),
> face arrière (suite 131), depth D405, `HeatmapRenderer` instancié mais dormant.
>
> Aucun code modifié — document de propositions uniquement. Matrice de priorisation en §5.

---

## Thème A — Inspection sans modèle IA (exploiter la pose + la D405 dès aujourd'hui)

> Le modèle `component_detector.onnx` reste le déverrouilleur P0, mais son entraînement est du
> temps GPU, pas du temps de session. Ces trois features livrent de la **détection de défauts
> réelle sans aucun modèle**, uniquement avec l'homographie (déjà solide) et la depth (déjà captée).

### A1. Scan mosaïque de la carte (board stitching) ⭐ la brique-mère

**Quoi** : pendant que l'utilisateur balaie la carte (comme il le fait déjà pour la couverture
verte de la minimap), accumuler chaque frame **warpée en espace carte** (inverse de l'homographie
courante, `Homography::pcbToImage` inversée) dans un canvas haute résolution (~10-15 px/mm,
choisi selon la RAM). Résultat : une image complète et orthorectifiée de la carte, bien au-delà
de la résolution d'une seule vue.

**Valeur** :
- Export PNG/JPEG documentaire (« photo d'usine » de la carte inspectée) — livrable immédiat.
- **Prérequis direct de A2 (golden diff)** et améliore le rapport (2.2 IDEES, déjà câblé).
- Gratuit en UX : la couverture minimap (suite 145) guide déjà le balayage ; même geste, deux sorties.

**Esquisse technique** :
- Nouveau `features/BoardScanner` sur QThread dédié (pattern `DatasetCreator`) : reçoit
  `(FrameRef, cv::Mat H)` depuis le handler `homographyUpdated` existant, `cv::warpPerspective`
  vers le canvas avec un masque de blending « meilleure netteté gagne » (la variance Laplacien
  par tuile existe déjà dans les gates DatasetCreator) — pas de seam-blending compliqué en V1.
- Gate qualité : n'accumuler que si tracking sain (mêmes seuils que `setTrackingQuality`,
  <15 inliers / reproj >6 px = skip) — évite d'imprimer une frame mal posée dans la mosaïque.
- Canvas par face (Front/Back — la convention miroir de la suite 131 s'applique).
- UI : bouton « Scan board » dans ControlPanel + affichage de la mosaïque dans la grande vue
  minimap (touche M) en fond optionnel, export via menu File.

**Effort** : 1 session. **Dépend de** : rien (tout est en place). **Risque** : RAM du canvas sur
très grande carte → clamp px/mm dynamique.

### A2. Diff « golden board » — détection de défauts par comparaison ⭐ la feature produit

**Quoi** : enregistrer la mosaïque A1 d'une carte de référence validée (« golden »). Sur chaque
carte suivante du même iBOM : re-scanner (ou comparer live, tuile par tuile, dès que le tracking
couvre une zone) et calculer une carte de différence alignée → composant manquant, retourné,
tombé, pont de soudure grossier ressortent en anomalies, **sans aucun modèle entraîné**.

**Valeur** : c'est le raccourci vers la promesse « inspection » du projet pendant que le dataset
IA se construit. En prime, les zones d'anomalie sont d'excellents **hard examples** à verser au
DatasetCreator (Phase D).

**Esquisse technique** :
- Comparaison en espace carte (les deux images sont déjà orthorectifiées par A1) : SSIM ou
  diff absolu par tuile 64×64 après égalisation locale d'illumination (ratio de moyennes par
  tuile — l'éclairage atelier varie, leçon des suites 138-141).
- Agrégation **par composant** via `ComponentMap` (requêtes spatiales existantes) : score
  d'anomalie par bbox iBOM → liste triée dans le BomPanel + marqueurs sur l'overlay.
- **Câble enfin `HeatmapRenderer`** (instancié depuis toujours, jamais nourri —
  INVESTIGATION §3.3) : la carte de diff EST sa raison d'être.
- Golden stocké sous `$IBOM_DATA_DIR/golden/<hash-pcbdata>/{front,back}.png`.

**Effort** : 1-1,5 session après A1. **Dépend de** : A1. **Risque** : faux positifs sur variations
d'éclairage → seuils par tuile + revue humaine (la liste triée, pas un verdict automatique).

### A3. Inspection 3D par depth D405 — composants absents/soulevés

**Quoi** : `m_lastDepthFrame` ne sert aujourd'hui qu'à l'Auto-Align (`Application.cpp`, prior
d'échelle). Avec la pose verrouillée, projeter la bbox de chaque composant visible dans la depth
et comparer la hauteur médiane mesurée au plan de la carte : un condensateur absent = zéro
relief ; un connecteur soulevé/tombé = relief anormal. Détection binaire présent/absent fiable
sur tout ce qui dépasse ~1 mm, sans IA.

**Valeur** : couvre exactement le cas que le futur détecteur 2D couvrira le moins bien (occlusion,
composants noirs sur masque noir), et alimente la colonne « Placed » automatiquement en mode
pick&place (vérification après pose).

**Esquisse technique** :
- Fit du plan carte par RANSAC sur la depth de la zone carte (le quad projeté est connu) — déjà
  évoqué comme idée (INVESTIGATION 2.4) mais jamais spécifié : ici il devient la référence z=0.
- Par composant visible : hauteur médiane dans la bbox érodée − plan ; comparaison à une hauteur
  attendue par footprint (table optionnelle `resources/footprint_heights.json`, sinon simple
  seuil « > 0,4 mm = présent »).
- Sortie : badge présent/absent/incertain dans BomPanel + option « auto-check Placed on P&P ».
- Prototyper **hors app** d'abord sur des captures depth réelles (recommandation déjà actée pour
  les idées depth).

**Effort** : 1 session de proto + 1 de câblage. **Dépend de** : D405 branchée (chemin RealSense
existant). **Risque** : bruit depth à courte distance sur composants < 0402 → assumer « gros
composants seulement » en V1.

---

## Thème B — Workflow d'inspection guidée

### B1. Tour guidé d'inspection (« guided tour ») ⭐ quick win à fort effet

**Quoi** : un mode « Inspection guidée » qui transforme la liste BOM filtrée en **itinéraire** :
tri par plus proche voisin (les positions sont connues), le composant courant est ciblé sur la
minimap et l'overlay, `markPlaced()` avance automatiquement au suivant et recentre la vue
(la homographie sait où il est). Barre de progression réutilisant `refreshInspectionStats`
(suite 130).

**Valeur** : c'est l'extension naturelle du P&P assisté (INVESTIGATION 6.3a) en vrai mode
produit : au microscope, les deux mains sur la carte, plus aucun aller-retour souris entre deux
composants. Combiné au marquage Placed persistant (session 3.3 IDEES, fait), une carte de 300
composants se traite en flux continu.

**Esquisse technique** : ordre = plus proche voisin glouton sur les centres (suffisant, pas
besoin de TSP exact) recalculé au changement de filtre ; réutilise `onComponentSelected`
(lambda extraite en suite 145) et le recentrage minimap existant ; raccourcis « composant
suivant/précédent » (N/P) + gros bouton « Placé ✓ » ; état = simple index dans la liste triée,
persisté avec la session d'inspection.

**Effort** : ½-1 session. **Dépend de** : rien. **Risque** : quasi nul — pur GUI/logique.

### B2. Annotations épinglées par composant

**Quoi** : clic droit sur un composant (CameraView, BomPanel ou minimap) → « Ajouter une note » :
texte + snapshot optionnel (crop de la frame courante autour du composant, la homographie donne
le cadrage). Marqueur 📌 sur l'overlay et la minimap, colonne « Notes » dans le BomPanel,
injection automatique dans le rapport HTML/PDF (ReportGenerator déjà branché).

**Valeur** : aujourd'hui un défaut constaté se note sur papier ou se perd. Avec A2/A3 qui vont
*générer* des suspicions, il faut un endroit où les consigner et les livrer — c'est la moitié
« traçabilité » du produit d'inspection.

**Esquisse technique** : `$IBOM_DATA_DIR/annotations/<hash-pcbdata>.json`
(ref → [{texte, timestamp, face, chemin snapshot}]) ; le hash pcbdata plutôt que le chemin
fichier (même clé que la proposition multi-carte §5 INVESTIGATION — les deux features partagent
ce keying, à implémenter une fois).

**Effort** : 1 session. **Dépend de** : rien. **Risque** : faible.

### B3. Undo/redo + journal d'audit des marquages

**Quoi** : pile undo/redo (Ctrl+Z) sur les actions d'inspection (Placed/Sourced, notes) + journal
append-only horodaté (`inspection_log.csv` : timestamp, ref, action, face) exportable.

**Valeur** : un mis-clic « Placed » en plein tour guidé B1 est aujourd'hui irréversible
silencieusement ; le journal donne la traçabilité (qui/quand) que demandent les rapports qualité.

**Effort** : ½ session. **Dépend de** : mieux après B1. **Risque** : nul.

---

## Thème C — Données & multi-cartes

### C1. Diff de révisions iBOM (rework de mise à jour)

**Quoi** : charger deux iBOM (rev A = état actuel de la carte, rev B = cible) → diff structurel
par référence : composants **ajoutés / supprimés / valeur ou footprint changés**. Mode overlay
« rework » : les supprimés en rouge (à dessouder), les ajoutés en vert (à poser), les changés en
orange — et le tour guidé B1 enchaîne dessus.

**Valeur** : cas d'usage atelier réel (passer un proto de rev 2 à rev 3) qu'aucun outil iBOM
n'adresse en AR. Le parser et les structures existent, le diff est du pur `std::map` sur les
références.

**Esquisse technique** : `IBomProject` déjà autoportant → charger le second dans une instance
séparée, diff sur `reference` (clé) avec comparaison value/footprint/position ; nouveau statut
par composant consommé par OverlayRenderer (couleur) et BomPanel (filtre « rework only »).

**Effort** : 1 session. **Dépend de** : rien. **Risque** : références renumérotées entre revs →
prévoir un fallback d'appariement par (position ≈, footprint) avec revue.

### C2. Bibliothèque de cartes (multi-projet, hash-based)

**Quoi** : généraliser le keying par hash pcbdata (déjà proposé pour `SavedAlignment`,
INVESTIGATION §5) en une **bibliothèque** : écran « Mes cartes » listant les iBOM connus avec
vignette (rendu minimap), alignement sauvegardé par face, golden A2, annotations B2, progression
d'inspection — et association code-barres (le combo 2.3 IDEES s'y branche naturellement).

**Valeur** : l'atelier qui alterne 3 cartes retrouve tout son état en un clic ; c'est le
« projet » qui manque à l'app, et le socle commun que A2/B2/3.3 réclament chacun de leur côté
(un seul keying hash à implémenter au lieu de trois).

**Effort** : 1 session (le gros est du rangement de persistance existante). **Dépend de** :
aucun, mais **à faire avant** A2/B2 pour ne pas créer trois conventions de stockage divergentes.

---

## Thème D — Assistance temps réel à la prise de vue

### D1. Conseiller de scène (exposition / reflets / netteté) ⭐ leçon directe du terrain

**Quoi** : les suites 138-141 ont montré que le levier n°1 des échecs d'Auto-Align est
**physique** : scène sous-exposée, gros reflet spéculaire, flou. Détecter ces trois conditions en
continu (histogramme de la zone carte, blobs saturés persistants, variance Laplacien déjà
calculée pour le focus assist) et afficher un bandeau actionnable : « ⚠ Reflet détecté en bas à
gauche — déplacez l'éclairage », « Scène sombre — Auto-Align dégradé ».

**Valeur** : transforme la recommandation orale (« tuer le reflet, cadrer plus serré ») en
garde-fou produit ; réduit les tickets « l'Auto-Align ne marche pas » dont la cause est la scène.
Peut aussi **gater l'Auto-Align** : proposer de corriger avant de lancer, plutôt que d'échouer en
`ambiguous`.

**Esquisse technique** : tout se calcule sur la frame downscalée déjà disponible dans le worker ;
reflets = composantes connexes >98 % de luminance stables sur N frames dans le quad carte ;
publication via un signal `sceneQuality(flags, QString)` → status bar + minimap (bordure).

**Effort** : ½-1 session. **Dépend de** : rien. **Risque** : seuils à calibrer terrain — livrer
en « conseil » (jamais bloquant) en V1.

### D2. Loupe locale (magnifier) dans CameraView

**Quoi** : maintenir une touche (ou clic molette) → loupe circulaire ×2-4 sous le curseur,
rendue depuis la frame pleine résolution (pas un zoom du widget — la source 1080p a plus de
détail que l'affichage fit). Overlay iBOM re-warpé dans la loupe.

**Valeur** : vérifier une soudure ou lire un marquage sans perdre le cadrage global ni toucher au
zoom/pan — le geste « je regarde de près 2 secondes » du travail au microscope.

**Esquisse technique** : dans `paintEvent`, second passage : crop source autour de
`mapToImage(cursor)`, drawImage dans un cercle clipé + le même `imageToWidgetT` recalé — les
transforms existent toutes (vérifiées en suite 139).

**Effort** : ½ session. **Dépend de** : rien. **Risque** : nul (pur affichage).

---

## 5. Matrice de priorisation

**Impact** : valeur produit pour l'usage atelier/microscope. **Effort** : ½s = demi-session.
Les ⭐ marquent le trio recommandé si une seule passe est budgétée.

| Prio | Réf | Feature | Impact | Effort | Dépend de |
|------|-----|---------|--------|--------|-----------|
| **P0** | B1 ⭐ | Tour guidé d'inspection | Fort (workflow quotidien) | ½-1s | rien |
| **P0** | D1 ⭐ | Conseiller de scène | Fort (fiabilise l'Auto-Align terrain) | ½-1s | rien |
| **P1** | C2 | Bibliothèque de cartes (hash) | Fort (socle commun A2/B2) | 1s | rien — avant A2/B2 |
| **P1** | A1 ⭐ | Scan mosaïque | Fort (livrable + prérequis A2) | 1s | rien |
| **P1** | A2 | Golden board diff | Majeur (défauts sans IA) | 1-1,5s | A1, C2 |
| **P2** | B2 | Annotations épinglées | Moyen (traçabilité) | 1s | C2 |
| **P2** | D2 | Loupe locale | Moyen (confort microscope) | ½s | rien |
| **P2** | B3 | Undo/redo + audit CSV | Moyen | ½s | B1 |
| **P3** | C1 | Diff de révisions iBOM | Moyen (niche mais unique) | 1s | rien |
| **P3** | A3 | Inspection 3D depth | Moyen (proto d'abord) | 2s | D405, proto hors app |

**Ordre recommandé** : B1 + D1 d'abord (aucune dépendance, effet immédiat sur l'usage réel et
sur la fiabilité perçue de l'Auto-Align), puis C2 → A1 → A2 qui forment la chaîne « inspection
par comparaison » — le vrai saut produit en attendant le modèle IA. B2/B3/D2 s'intercalent en
demi-sessions. A3 attend un proto depth concluant.

**Rappel** : rien ici ne remplace le P0 permanent de la matrice INVESTIGATION §11 —
**entraîner `component_detector.onnx`** reste le déverrouilleur (temps GPU, pas temps de
session) ; A2 et A3 sont précisément conçus pour livrer de la valeur d'inspection *pendant* que
le dataset se construit, et lui fournir des hard examples.

---

*Analyse du 2026-07-12 sur `main` @ `4b248df` (branche `claude/new-feature-proposals-zfqkfl`).
Aucun code modifié — document de propositions uniquement.*
