# Tuto pour les nuls — quels datasets pour le component_detector ?

> **Tu m'as donné 3 liens, la réponse courte : OUI aux 3, et le mieux est de les
> FUSIONNER en un seul dataset « présence ».** Ce document explique comment, pas
> à pas, sans rien coder.
>
> Ce doc = **le choix des données**. Pour la suite (entraîner + exporter + poser
> sur le Jetson), c'est [TUTO_MODELE_REANCRAGE.md](TUTO_MODELE_REANCRAGE.md).
> Le fond technique (pourquoi présence suffit) : [AI_MODEL_DATASETS_PLAN.md](AI_MODEL_DATASETS_PLAN.md)
> et [AUTO_ALIGN_V2_PLAN.md](AUTO_ALIGN_V2_PLAN.md).

---

## 1. Réponse directe à tes 3 liens

| Ton lien | Peux-tu l'utiliser ? | Comment le récupérer | Qualité pour NOUS |
|---|---|---|---|
| **roboflow · pcb-components-tqghw / pcb-component-detection-v2-zcun5** | ✅ **Oui — le meilleur des 3 pour démarrer** | Notre script Roboflow (1 commande) | 🟡 Moyen : composants séparés, directement exploitable |
| **roboflow · roboflow-100 / printed-circuit-board** | ✅ Oui, mais **en renfort seulement** | Notre script Roboflow (1 commande) | ❌ Faible seul (labels grossiers, niveau carte) — utile juste pour ajouter du volume |
| **kaggle · aryanstein / pcb-component-detection-consolidated-dataset** | ✅ Oui — **potentiellement le plus gros** | Kaggle (PAS notre script Roboflow) — voir §4 | ❓ « consolidated » = fusion de plusieurs sources → beaucoup d'images, à vérifier (format + licence) |

**Le point clé** : pour le **re-ancrage** (ce qu'on veut : aligner l'overlay), on
n'a **pas besoin de connaître le type** de chaque composant. On a juste besoin de
savoir **où** ils sont. Donc un modèle **« présence »** (1 seule classe :
« il y a un composant ici ») suffit — et notre outil `remap_classes.py --presence`
**écrase toutes les classes en une seule**. Conséquence énorme :

> **Peu importe que tes 3 datasets aient des noms de classes différents,
> incompatibles, en désordre. En mode présence, tout devient « component ».
> Donc tu peux tous les empiler pour avoir un maximum d'images.**

C'est exactement ce que fait le §3.

---

## 2. Avant tout — sur quelle machine ?

D'après le journal, tu as un **serveur Ubuntu avec une RTX 5070 Ti**. C'est LÀ
qu'on entraîne (pas le Jetson — trop lent). Le Jetson ne fait que lancer l'app à
la fin. Toutes les commandes ci-dessous se tapent **sur le serveur**.

---

## 3. La méthode recommandée : fusionner les 3 (présence)

C'est le chemin le plus fort : plus tu as d'images variées, meilleur est le
détecteur. On récupère les 3, on les fusionne en « présence », on entraîne une
fois.

### 3.1 Récupérer les 2 Roboflow (script existant, 1 commande chacun)

```bash
# sur le serveur, à la racine du repo, avec ta clé Roboflow :
export ROBOFLOW_API_KEY=ta_cle_roboflow

python3 scripts/fetch_roboflow_dataset.py \
    pcb-components-tqghw/pcb-component-detection-v2-zcun5 \
    --out datasets/rf_components

python3 scripts/fetch_roboflow_dataset.py \
    roboflow-100/printed-circuit-board \
    --out datasets/rf100_pcb
```

> Le script accepte soit l'URL complète, soit le raccourci `workspace/project`
> (montré ici). La clé API se prend sur app.roboflow.com → Settings → API Keys.

### 3.2 Récupérer le Kaggle (§4 ci-dessous, une fois)

Résultat attendu : un dossier `datasets/kaggle_consolidated/` au **format YOLO**
(sous-dossiers `images/` et `labels/`, un `data.yaml`). Si le format est
différent, voir §4.3.

### 3.3 Remapper CHAQUE source en « présence » (étape à ne PAS sauter)

Les 3 datasets ont des classes différentes (l'un numérote les résistances 0,
l'autre 3, etc.). `remap_classes.py --presence` **réécrit tous les IDs à 0**
(= 1 seule classe « component ») dans chaque dossier, sur place :

```bash
python3 scripts/remap_classes.py datasets/rf_components       --presence
python3 scripts/remap_classes.py datasets/rf100_pcb           --presence
python3 scripts/remap_classes.py datasets/kaggle_consolidated --presence
```

> ⚠️ **Indispensable.** L'étape de fusion (§3.4) **ne touche PAS** aux IDs dans
> les fichiers `.txt` — elle ne fait que copier. Si tu sautes ce remap, tu te
> retrouves avec des IDs 0,1,2,… dans les labels mais un `data.yaml` qui dit
> « 1 classe » → l'entraînement plante ou apprend n'importe quoi.

### 3.4 Fusionner les 3 en un seul dataset « présence »

```bash
python3 scripts/merge_datasets.py \
    datasets/rf_components datasets/rf100_pcb datasets/kaggle_consolidated \
    --presence \
    --out datasets/merged_presence
```

Ça produit `datasets/merged_presence/` avec `data.yaml` (1 classe `component`),
un split train/val déterministe, et les images/labels de toutes les sources
mélangés (préfixés par source pour éviter les collisions de noms).

> Si un des 3 dossiers coince (format bizarre, 0 paire trouvée), **retire-le de
> la ligne** — même 2 sources sur 3, ça marche. Le Roboflow `pcb-component-
> detection-v2` seul suffit déjà pour un premier modèle utile.

### 3.5 Entraîner + exporter (le reste du tuto existant)

```bash
python3 scripts/train_yolo.py datasets/merged_presence/data.yaml \
    --model yolov8n.pt --epochs 100 --batch 32 --device 0 --name reanchor_presence

python3 scripts/export_yolov8_onnx.py \
    runs/detect/reanchor_presence/weights/best.pt \
    --out models/component_detector.onnx
```

Puis tu copies `models/component_detector.onnx` (+ le `.txt` à côté) sur le
Jetson, dans `models/`, et tu relances l'app. Détails et dépannage :
[TUTO_MODELE_REANCRAGE.md](TUTO_MODELE_REANCRAGE.md) étapes 5-7.

> **Encore plus simple** : `scripts/train_on_server.sh` enchaîne fetch → remap →
> train → export tout seul, MAIS il ne prend qu'**une** source Roboflow (le
> `pcb-component-detection-v2`). Si tu veux juste un modèle vite fait sans
> fusionner, édite `DATASET_URL` dans ce script pour mettre ton lien v2 et lance
> `bash scripts/train_on_server.sh`. Pour profiter des 3 datasets, suis le §3
> manuel ci-dessus.

---

## 4. Cas particulier : le dataset Kaggle

Notre `fetch_roboflow_dataset.py` **ne sait parler qu'à Roboflow**. Kaggle a son
propre système. Deux façons :

### 4.1 Le plus simple — téléchargement manuel (navigateur)

1. Va sur la page Kaggle du dataset, connecte-toi (compte gratuit).
2. Bouton **Download** (en haut) → tu récupères un `.zip`.
3. Sur le serveur, décompresse-le dans `datasets/kaggle_consolidated/` :
   ```bash
   mkdir -p datasets/kaggle_consolidated
   unzip ~/Downloads/archive.zip -d datasets/kaggle_consolidated
   ```

### 4.2 En ligne de commande (kaggle CLI)

```bash
pip install kaggle
# récupère ton token : kaggle.com → ton avatar → Settings → API → "Create New Token"
# ça télécharge kaggle.json ; place-le ici :
mkdir -p ~/.kaggle && mv ~/Downloads/kaggle.json ~/.kaggle/ && chmod 600 ~/.kaggle/kaggle.json

kaggle datasets download -d aryanstein/pcb-component-detection-consolidated-dataset \
    -p datasets/kaggle_consolidated --unzip
```

### 4.3 Vérifier le format après extraction

Regarde ce qu'il y a dedans :
```bash
find datasets/kaggle_consolidated -maxdepth 2 -type d
ls datasets/kaggle_consolidated
```

- **Si tu vois `images/` + `labels/` avec des `.txt` + un `data.yaml`** → c'est du
  YOLO, `merge_datasets.py` le prend tel quel (§3.3). 👍
- **Si tu vois des `.xml`** (Pascal VOC) ou un `.json` COCO → il faut convertir en
  YOLO d'abord. Le plus simple : ré-uploade le dossier sur Roboflow (import
  gratuit), il convertit et ré-exporte en YOLOv8, puis tu le fetch comme les
  autres. Sinon, dis-le-moi et j'ajoute un petit convertisseur.

### 4.4 ⚠️ Licence

Un dataset « consolidated » agrège d'autres datasets → **vérifie la licence sur
la page Kaggle** avant de **distribuer** un modèle entraîné dessus. Pour ton
usage **personnel/interne** (aligner ton overlay sur ta machine), c'est sans
souci. La diffusion publique des poids, c'est ça qu'il faut vérifier.

---

## 5. Le seul truc qui compte vraiment

Tant que `models/component_detector.onnx` **n'existe pas** sur le Jetson, tout le
chemin détection (Auto-Align qui marche carte plein cadre, ré-alignement
automatique quand tu bouges la carte) reste **endormi** — l'app retombe sur
l'ancien Auto-Align géométrique.

**Dès que ce fichier est en place**, tout s'allume. Donc : n'importe lequel des 3
datasets te donne déjà un modèle qui débloque tout. Commence par le plus simple
(le Roboflow `pcb-component-detection-v2` seul via `train_on_server.sh`), vérifie
que ça marche sur la carte, **puis** refais avec les 3 fusionnés pour gagner en
robustesse. Ne bloque pas sur « le dataset parfait » : un modèle présence moyen
bat largement pas de modèle du tout.

---

## 6. Résumé en 5 lignes

1. Les 3 datasets sont utilisables. Le meilleur pour démarrer = le Roboflow v2.
2. Pour le re-ancrage, on veut la **position**, pas le type → mode **présence**.
3. En présence, on peut **empiler les 3** (leurs classes n'ont pas à correspondre).
4. Roboflow = notre script ; Kaggle = `kaggle` CLI ou download manuel, puis fusion.
5. Objectif = **produire `models/component_detector.onnx`**. Un modèle moyen suffit à tout débloquer.
