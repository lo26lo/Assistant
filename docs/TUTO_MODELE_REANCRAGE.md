# Tuto pas-à-pas — créer le modèle de re-ancrage (pour débutant)

> **But** : produire un fichier `component_detector.onnx`, le poser sur le Jetson,
> et activer le **re-ancrage automatique** de l'overlay (celui qui marche même
> quand la carte remplit l'écran, contrairement à l'Auto-Align géométrique).
>
> On fait **la Piste B d'abord** (la plus rapide). La Piste A (détecteur fin
> 14 classes) est tout en bas — même principe, juste plus de données.
>
> Tu n'as **rien à coder**. Tu tapes des commandes, c'est tout.

---

## Vue d'ensemble (les 6 étapes)

```
1. Compte Roboflow + clé API   (5 min, navigateur)
2. Installer les outils Python (5 min, une fois)
3. Télécharger un dataset      (2 min)
4. Le préparer (remap)         (1 min)
5. Entraîner le modèle         (30 min – 2 h selon le GPU)
6. Exporter en .onnx           (2 min)
   -> copier sur le Jetson + relancer l'app
```

**Sur quelle machine ?** Fais les étapes 2 à 6 sur le **PC portable Windows
avec la RTX 5070** (il a déjà CUDA + Python). Le Jetson, lui, ne fait que
**lancer l'app** (étape finale). N'entraîne PAS sur le Jetson, ce serait lent.

> 💡 Pas envie d'installer quoi que ce soit ? Voir l'annexe « Google Colab » à la
> fin : tout se fait dans le navigateur, gratuitement.

---

## Étape 1 — Compte Roboflow + clé API

1. Va sur **https://app.roboflow.com** → crée un compte gratuit (ou connecte-toi).
2. En haut à gauche, clique sur le nom de ton espace de travail → **Settings**.
3. Ouvre la section **API Keys**. Copie ta **Private API Key** (une longue suite
   de lettres/chiffres). **Garde-la secrète** (ne la mets jamais sur GitHub).

C'est tout pour le navigateur.

---

## Étape 2 — Installer les outils (une seule fois)

Ouvre **PowerShell** sur le PC Windows et tape, ligne par ligne :

```powershell
cd C:\Users\bambo\Assistant\Assistant
pip install ultralytics roboflow pyyaml
```

> Ça télécharge YOLO (l'outil d'entraînement) et le client Roboflow.
> Si `pip` n'est pas reconnu, remplace par : `C:\Data\Python312\python.exe -m pip install ultralytics roboflow pyyaml`

**Vérifie que ça a marché** :
```powershell
python -c "import ultralytics, roboflow, yaml; print('OK tout est installe')"
```
Tu dois voir `OK tout est installe`.

---

## Étape 3 — Télécharger le dataset

D'abord, donne ta clé API à la session (remplace `COLLE_TA_CLE_ICI`) :

```powershell
$env:ROBOFLOW_API_KEY = "COLLE_TA_CLE_ICI"
```

Puis télécharge le dataset SMD (le plus adapté à nos cartes) :

```powershell
python scripts\fetch_roboflow_dataset.py `
    https://universe.roboflow.com/marco-filippozzi-siwjn/smd-component-detection `
    --out datasets\smd
```

**Ce que tu dois voir** : des lignes `[fetch] ...` puis `[fetch] OK -> ...`.
Un dossier `datasets\smd` est créé, avec des images dedans.

> ❗ Si tu as une erreur de licence ou d'accès : ouvre la page du dataset dans le
> navigateur, clique **Download** une fois (ça « accepte » le dataset sur ton
> compte), puis relance la commande.

---

## Étape 4 — Préparer le dataset (« remap »)

Pour la Piste B, on transforme **toutes les classes en une seule** (« composant ») —
c'est tout ce dont le re-ancrage a besoin (il repère des **positions**, pas des
types).

```powershell
python scripts\remap_classes.py datasets\smd --presence
```

**Ce que tu dois voir** : `[remap] N fichiers de labels réécrits.` et
`[remap] data.yaml mis à jour : nc=1 names=['component']`.

---

## Étape 5 — Entraîner le modèle

```powershell
python scripts\train_yolo.py datasets\smd\data.yaml `
    --model yolov8n.pt --epochs 80 --name reanchor_presence
```

> `yolov8n` = le plus petit/rapide. `--epochs 80` = nombre de passes
> d'entraînement. Ça tourne 30 min à 2 h selon le GPU. Tu peux laisser tourner.

**Ce que tu dois voir** : une barre de progression par époque. À la fin, un message
`[train] terminé. Meilleur checkpoint attendu : runs\detect\reanchor_presence\weights\best.pt`.

> Si le PC manque de mémoire GPU, ajoute `--batch 8` (ou `--batch 4`) à la commande.

---

## Étape 6 — Exporter en .onnx

```powershell
python scripts\export_yolov8_onnx.py `
    runs\detect\reanchor_presence\weights\best.pt `
    --out models\component_detector.onnx
```

**Ce que tu dois voir** : `models\component_detector.onnx` créé, **plus** un fichier
`models\component_detector.txt` à côté (la liste des classes). Les deux sont
nécessaires.

---

## Étape 7 — Mettre le modèle sur le Jetson et tester

1. **Copie** les deux fichiers `component_detector.onnx` et
   `component_detector.txt` dans le dossier `models/` du projet **sur le Jetson**
   (clé USB, `scp`, partage réseau… comme tu préfères).
2. **Relance l'application** sur le Jetson.
   - ⏳ Au **tout premier lancement avec le modèle**, l'app compile un « engine »
     TensorRT : ça prend **quelques minutes**, une seule fois. C'est normal.
3. **Charge ta carte + ton iBOM**, fais **un** alignement de départ (Auto-Align ou
   manuel) pour avoir une pose initiale.
4. **Teste** : menu **Dev → « Component re-anchor now »**. L'overlay doit se
   recaler sur les composants.
5. Pour que ça se fasse **tout seul** pendant le suivi : laisse **Auto re-anchor**
   coché dans les réglages. Dès qu'un modèle est présent, le re-ancrage périodique
   utilise automatiquement l'IA (et ça marche carte en plein cadre).

**Comment savoir que ça marche ?** Active **Dev → Verbose debug logging**, et
cherche dans le log les lignes `[comp-reanchor] re-anchored on …/… components`.

---

## Et après ? Piste A (modèle fin 14 classes) — plus tard

Quand la Piste B tourne, tu peux viser un modèle qui **nomme** les composants
(résistance, condensateur, QFN…). Le principe est le même, en 3 différences :

1. **Capturer tes propres cartes** dans l'app (panneau Dataset) → ça crée des
   dossiers `session_*` déjà étiquetés dans nos 14 classes.
2. **Mapper** les classes du dataset public vers nos 14 (au lieu de `--presence`) :
   ```powershell
   python scripts\remap_classes.py datasets\smd --map mon_mapping.yaml
   ```
   (pars de `scripts\class_mapping.example.yaml`, je peux te le remplir.)
3. **Fusionner** public + tes sessions, puis entraîner un modèle moyen :
   ```powershell
   python scripts\merge_datasets.py --out datasets\merged datasets\smd <dossier_session>
   python scripts\train_yolo.py datasets\merged\data.yaml --model yolov8m.pt --epochs 150 --name component_detector
   ```
Le reste (export, copie Jetson) est identique.

---

## Annexe — Tout faire dans le navigateur (Google Colab, zéro installation)

Si tu ne veux rien installer sur Windows :
1. Va sur **https://colab.research.google.com** → nouveau notebook.
2. Menu **Exécution → Modifier le type d'exécution → GPU**.
3. Dans une cellule, tape (puis ▶) :
   ```python
   !git clone https://github.com/lo26lo/Assistant.git
   %cd Assistant
   !pip install ultralytics roboflow pyyaml
   import os; os.environ["ROBOFLOW_API_KEY"] = "COLLE_TA_CLE_ICI"
   !python scripts/fetch_roboflow_dataset.py https://universe.roboflow.com/marco-filippozzi-siwjn/smd-component-detection --out datasets/smd
   !python scripts/remap_classes.py datasets/smd --presence
   !python scripts/train_yolo.py datasets/smd/data.yaml --model yolov8n.pt --epochs 80 --name reanchor_presence
   !python scripts/export_yolov8_onnx.py runs/detect/reanchor_presence/weights/best.pt --out models/component_detector.onnx
   ```
4. Télécharge le résultat : dans le panneau **Fichiers** (icône dossier à gauche),
   va dans `Assistant/models/`, clic droit sur `component_detector.onnx` et `.txt`
   → **Télécharger**. Puis reprends à l'**Étape 7**.

> ❗ Si le dépôt est **privé**, le `git clone` ci-dessus échouera. Dans ce cas,
> glisse-dépose simplement les 4 fichiers du dossier `scripts/` (fetch / remap /
> train / export) dans le panneau **Fichiers** de Colab, et lance les commandes
> sans le `git clone` ni le `%cd`.

---

## Aide-mémoire des problèmes courants

| Symptôme | Cause probable | Solution |
|---|---|---|
| `pip: command not found` | Python pas dans le PATH | `C:\Data\Python312\python.exe -m pip ...` |
| Erreur d'accès au téléchargement | Dataset pas « accepté » sur ton compte | Ouvre sa page, clique Download une fois |
| `CUDA out of memory` à l'entraînement | Batch trop gros | Ajoute `--batch 8` (ou 4) |
| L'app ne détecte rien après copie | `.txt` oublié, ou mauvais nom | Les **deux** fichiers, nommés `component_detector.*`, dans `models/` |
| Premier lancement très long | Compilation engine TensorRT | Normal, une seule fois, attends quelques minutes |
| Re-ancrage ne bouge pas | Pas de pose de départ | Fais d'abord un Auto-Align/alignement manuel |
