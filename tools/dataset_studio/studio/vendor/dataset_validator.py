# =============================================================================
#  Vendorise depuis Pokemon-Dataset-Creator (github.com/lo26lo/Pokemon-Dataset-Creator)
#  core/dataset_validator.py — module generique de validation YOLO, repris
#  tel quel (seul l'import de safe_print pointe vers notre vendor/utils.py).
# =============================================================================
#!/usr/bin/env python3
"""
Module de validation de dataset YOLO
Vérifie la qualité des annotations, détecte les problèmes, génère des rapports
"""
import os
import cv2
import numpy as np
from pathlib import Path
from collections import defaultdict
import json
from datetime import datetime

# Import safe_print pour gérer l'encodage Unicode sur Windows
try:
    from .utils import safe_print
except ImportError:
    from utils import safe_print


class DatasetValidator:
    """Validateur de dataset YOLO avec rapport détaillé"""
    
    def __init__(self, dataset_dir):
        """
        Initialise le validateur
        
        Args:
            dataset_dir: Chemin vers le dossier contenant images/ et labels/
        """
        self.dataset_dir = Path(dataset_dir)
        self.images_dir = self.dataset_dir / "images"
        self.labels_dir = self.dataset_dir / "labels"
        
        self.results = {
            'total_images': 0,
            'total_labels': 0,
            'corrupted_images': [],
            'missing_labels': [],
            'invalid_annotations': [],
            'class_distribution': defaultdict(int),
            'bbox_stats': {
                'widths': [],
                'heights': [],
                'areas': []
            },
            'annotations_out_of_bounds': [],
            'empty_annotations': [],
            'warnings': [],
            'errors': []
        }
    
    def validate(self):
        """Lance la validation complète du dataset"""
        safe_print("🔍 Démarrage de la validation du dataset...")
        safe_print(f"📁 Dataset: {self.dataset_dir}")
        safe_print()
        
        if not self.images_dir.exists():
            raise FileNotFoundError(f"❌ Dossier images/ non trouvé: {self.images_dir}")
        
        if not self.labels_dir.exists():
            raise FileNotFoundError(f"❌ Dossier labels/ non trouvé: {self.labels_dir}")
        
        # 1. Lister toutes les images
        image_files = list(self.images_dir.glob("*.png")) + \
                      list(self.images_dir.glob("*.jpg")) + \
                      list(self.images_dir.glob("*.jpeg"))
        
        self.results['total_images'] = len(image_files)
        safe_print(f"📊 {len(image_files)} images trouvées")
        
        # 2. Valider chaque image
        for idx, img_path in enumerate(image_files, 1):
            if idx % 100 == 0:
                safe_print(f"   Progression: {idx}/{len(image_files)} images...")
            
            self._validate_image(img_path)
        
        # 3. Analyser les résultats
        self._analyze_results()
        
        safe_print()
        safe_print("✅ Validation terminée!")
        
        return self.results
    
    def _validate_image(self, img_path):
        """Valide une image et son annotation"""
        # 1. Vérifier que l'image n'est pas corrompue
        try:
            img = cv2.imread(str(img_path))
            if img is None:
                self.results['corrupted_images'].append(str(img_path))
                self.results['errors'].append(f"Image corrompue: {img_path.name}")
                return
            
            img_h, img_w = img.shape[:2]
        except Exception as e:
            self.results['corrupted_images'].append(str(img_path))
            self.results['errors'].append(f"Erreur lecture {img_path.name}: {str(e)}")
            return
        
        # 2. Vérifier l'existence du fichier label
        label_path = self.labels_dir / (img_path.stem + ".txt")
        
        if not label_path.exists():
            self.results['missing_labels'].append(str(img_path))
            self.results['warnings'].append(f"Label manquant pour: {img_path.name}")
            return
        
        # 3. Valider les annotations
        try:
            with open(label_path, 'r') as f:
                lines = f.readlines()
            
            self.results['total_labels'] += 1
            
            if not lines:
                self.results['empty_annotations'].append(str(label_path))
                self.results['warnings'].append(f"Annotation vide: {label_path.name}")
                return
            
            for line_num, line in enumerate(lines, 1):
                line = line.strip()
                if not line:
                    continue
                
                try:
                    parts = line.split()
                    if len(parts) != 5:
                        self.results['invalid_annotations'].append({
                            'file': str(label_path),
                            'line': line_num,
                            'error': f"Format invalide: attendu 5 valeurs, reçu {len(parts)}"
                        })
                        continue
                    
                    class_id = int(parts[0])
                    x_center = float(parts[1])
                    y_center = float(parts[2])
                    width = float(parts[3])
                    height = float(parts[4])
                    
                    # Vérifier que les valeurs sont entre 0 et 1
                    if not (0 <= x_center <= 1 and 0 <= y_center <= 1 and 
                            0 <= width <= 1 and 0 <= height <= 1):
                        self.results['annotations_out_of_bounds'].append({
                            'file': str(label_path),
                            'line': line_num,
                            'values': (x_center, y_center, width, height)
                        })
                        self.results['errors'].append(
                            f"Valeurs hors limites dans {label_path.name}:{line_num}"
                        )
                    
                    # Statistiques par classe
                    self.results['class_distribution'][class_id] += 1
                    
                    # Statistiques de bounding boxes
                    self.results['bbox_stats']['widths'].append(width)
                    self.results['bbox_stats']['heights'].append(height)
                    self.results['bbox_stats']['areas'].append(width * height)
                    
                except ValueError as e:
                    self.results['invalid_annotations'].append({
                        'file': str(label_path),
                        'line': line_num,
                        'error': f"Valeurs non numériques: {str(e)}"
                    })
        
        except Exception as e:
            self.results['errors'].append(f"Erreur lecture label {label_path.name}: {str(e)}")
    
    def _analyze_results(self):
        """Analyse les résultats de validation"""
        # Calculer les statistiques de bounding boxes
        if self.results['bbox_stats']['widths']:
            widths = np.array(self.results['bbox_stats']['widths'])
            heights = np.array(self.results['bbox_stats']['heights'])
            areas = np.array(self.results['bbox_stats']['areas'])
            
            self.results['bbox_stats']['width_mean'] = float(np.mean(widths))
            self.results['bbox_stats']['width_std'] = float(np.std(widths))
            self.results['bbox_stats']['height_mean'] = float(np.mean(heights))
            self.results['bbox_stats']['height_std'] = float(np.std(heights))
            self.results['bbox_stats']['area_mean'] = float(np.mean(areas))
            self.results['bbox_stats']['area_std'] = float(np.std(areas))
        
        # Détecter les classes sous-représentées
        if self.results['class_distribution']:
            total_annotations = sum(self.results['class_distribution'].values())
            min_threshold = 10  # Minimum recommandé par classe
            
            for class_id, count in self.results['class_distribution'].items():
                if count < min_threshold:
                    self.results['warnings'].append(
                        f"Classe {class_id} sous-représentée: {count} annotations "
                        f"(recommandé: >{min_threshold})"
                    )
    
    def print_report(self):
        """Affiche un rapport de validation formaté"""
        safe_print()
        safe_print("=" * 60)
        safe_print("📊 RAPPORT DE VALIDATION DU DATASET")
        safe_print("=" * 60)
        safe_print()
        
        # Vue d'ensemble
        safe_print("📈 VUE D'ENSEMBLE")
        safe_print(f"   Images totales:     {self.results['total_images']}")
        safe_print(f"   Labels totaux:      {self.results['total_labels']}")
        safe_print(f"   Classes uniques:    {len(self.results['class_distribution'])}")
        
        if self.results['class_distribution']:
            total_annot = sum(self.results['class_distribution'].values())
            safe_print(f"   Annotations:        {total_annot}")
        
        safe_print()
        
        # Erreurs critiques
        errors = []
        if self.results['corrupted_images']:
            errors.append(f"{len(self.results['corrupted_images'])} images corrompues")
        if self.results['annotations_out_of_bounds']:
            errors.append(f"{len(self.results['annotations_out_of_bounds'])} annotations hors limites")
        if self.results['invalid_annotations']:
            errors.append(f"{len(self.results['invalid_annotations'])} annotations invalides")
        
        if errors:
            safe_print("❌ ERREURS CRITIQUES")
            for error in errors:
                safe_print(f"   • {error}")
            safe_print()
        else:
            safe_print("✅ AUCUNE ERREUR CRITIQUE")
            safe_print()
        
        # Avertissements
        if self.results['missing_labels']:
            safe_print(f"⚠️  AVERTISSEMENTS")
            safe_print(f"   • {len(self.results['missing_labels'])} images sans label")
        
        if self.results['empty_annotations']:
            safe_print(f"   • {len(self.results['empty_annotations'])} annotations vides")
        
        # Classes sous-représentées
        underrepresented = [w for w in self.results['warnings'] if 'sous-représentée' in w]
        if underrepresented:
            safe_print(f"   • {len(underrepresented)} classes sous-représentées")
        
        if self.results['missing_labels'] or self.results['empty_annotations'] or underrepresented:
            safe_print()
        
        # Distribution des classes
        if self.results['class_distribution']:
            safe_print("📊 DISTRIBUTION DES CLASSES")
            sorted_classes = sorted(
                self.results['class_distribution'].items(),
                key=lambda x: x[1],
                reverse=True
            )
            
            # Top 10 classes
            safe_print("   Top 10 classes les plus fréquentes:")
            for class_id, count in sorted_classes[:10]:
                bar = "█" * min(50, count // 2)
                safe_print(f"   Classe {class_id:3d}: {count:4d} | {bar}")
            
            if len(sorted_classes) > 10:
                safe_print(f"   ... et {len(sorted_classes) - 10} autres classes")
            
            safe_print()
        
        # Statistiques de bounding boxes
        if 'width_mean' in self.results['bbox_stats']:
            safe_print("📏 STATISTIQUES DES BOUNDING BOXES")
            stats = self.results['bbox_stats']
            safe_print(f"   Largeur moyenne:  {stats['width_mean']:.3f} ± {stats['width_std']:.3f}")
            safe_print(f"   Hauteur moyenne:  {stats['height_mean']:.3f} ± {stats['height_std']:.3f}")
            safe_print(f"   Aire moyenne:     {stats['area_mean']:.3f} ± {stats['area_std']:.3f}")
            safe_print()
        
        # Recommandations
        safe_print("💡 RECOMMANDATIONS")
        if not errors and not self.results['warnings']:
            safe_print("   ✅ Dataset prêt pour l'entraînement !")
        else:
            if self.results['corrupted_images']:
                safe_print("   • Supprimer ou réparer les images corrompues")
            if self.results['missing_labels']:
                safe_print("   • Ajouter les labels manquants ou supprimer les images")
            if underrepresented:
                safe_print("   • Augmenter les classes sous-représentées (augmentation)")
            if self.results['annotations_out_of_bounds']:
                safe_print("   • Corriger les annotations hors limites")
        
        safe_print()
        safe_print("=" * 60)
    
    def save_report_html(self, output_path="validation_report.html"):
        """Génère un rapport HTML avec graphiques"""
        html = f"""
<!DOCTYPE html>
<html>
<head>
    <title>Rapport de Validation Dataset</title>
    <meta charset="utf-8">
    <style>
        body {{
            font-family: 'Segoe UI', Arial, sans-serif;
            margin: 40px;
            background: #f5f5f5;
        }}
        .container {{
            max-width: 1200px;
            margin: 0 auto;
            background: white;
            padding: 30px;
            border-radius: 10px;
            box-shadow: 0 2px 10px rgba(0,0,0,0.1);
        }}
        h1 {{
            color: #2c3e50;
            border-bottom: 3px solid #3498db;
            padding-bottom: 10px;
        }}
        h2 {{
            color: #34495e;
            margin-top: 30px;
        }}
        .metric {{
            display: inline-block;
            background: #ecf0f1;
            padding: 15px 25px;
            margin: 10px;
            border-radius: 5px;
            border-left: 4px solid #3498db;
        }}
        .metric-value {{
            font-size: 32px;
            font-weight: bold;
            color: #2c3e50;
        }}
        .metric-label {{
            font-size: 14px;
            color: #7f8c8d;
            text-transform: uppercase;
        }}
        .error {{
            background: #e74c3c;
            color: white;
            padding: 10px;
            border-radius: 5px;
            margin: 5px 0;
        }}
        .warning {{
            background: #f39c12;
            color: white;
            padding: 10px;
            border-radius: 5px;
            margin: 5px 0;
        }}
        .success {{
            background: #27ae60;
            color: white;
            padding: 10px;
            border-radius: 5px;
            margin: 5px 0;
        }}
        .bar {{
            background: #3498db;
            height: 25px;
            margin: 5px 0;
            border-radius: 3px;
            display: flex;
            align-items: center;
            padding-left: 10px;
            color: white;
        }}
        table {{
            width: 100%;
            border-collapse: collapse;
            margin: 20px 0;
        }}
        th, td {{
            padding: 12px;
            text-align: left;
            border-bottom: 1px solid #ddd;
        }}
        th {{
            background: #34495e;
            color: white;
        }}
        .timestamp {{
            color: #95a5a6;
            font-size: 14px;
        }}
    </style>
</head>
<body>
    <div class="container">
        <h1>📊 Rapport de Validation du Dataset</h1>
        <p class="timestamp">Généré le {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}</p>
        
        <h2>Vue d'Ensemble</h2>
        <div>
            <div class="metric">
                <div class="metric-value">{self.results['total_images']}</div>
                <div class="metric-label">Images</div>
            </div>
            <div class="metric">
                <div class="metric-value">{self.results['total_labels']}</div>
                <div class="metric-label">Labels</div>
            </div>
            <div class="metric">
                <div class="metric-value">{len(self.results['class_distribution'])}</div>
                <div class="metric-label">Classes</div>
            </div>
            <div class="metric">
                <div class="metric-value">{sum(self.results['class_distribution'].values()) if self.results['class_distribution'] else 0}</div>
                <div class="metric-label">Annotations</div>
            </div>
        </div>
        
        <h2>Statut de Validation</h2>
"""
        
        # Erreurs et avertissements
        if not self.results['errors'] and not self.results['warnings']:
            html += '<div class="success">✅ Aucun problème détecté - Dataset prêt pour l\'entraînement !</div>'
        else:
            if self.results['errors']:
                html += f'<div class="error">❌ {len(self.results["errors"])} erreurs détectées</div>'
            if self.results['warnings']:
                html += f'<div class="warning">⚠️ {len(self.results["warnings"])} avertissements</div>'
        
        # Distribution des classes
        if self.results['class_distribution']:
            html += "<h2>Distribution des Classes</h2>"
            html += "<table><tr><th>Classe</th><th>Nombre d'annotations</th><th>Visualisation</th></tr>"
            
            sorted_classes = sorted(
                self.results['class_distribution'].items(),
                key=lambda x: x[1],
                reverse=True
            )
            
            max_count = sorted_classes[0][1] if sorted_classes else 1
            
            for class_id, count in sorted_classes[:20]:  # Top 20
                bar_width = int((count / max_count) * 100)
                html += f"<tr><td>Classe {class_id}</td><td>{count}</td><td><div class='bar' style='width:{bar_width}%'>{count}</div></td></tr>"
            
            html += "</table>"
        
        # Statistiques des bounding boxes
        if 'width_mean' in self.results['bbox_stats']:
            stats = self.results['bbox_stats']
            html += "<h2>Statistiques des Bounding Boxes</h2>"
            html += f"""
            <table>
                <tr><th>Métrique</th><th>Moyenne</th><th>Écart-type</th></tr>
                <tr><td>Largeur</td><td>{stats['width_mean']:.3f}</td><td>{stats['width_std']:.3f}</td></tr>
                <tr><td>Hauteur</td><td>{stats['height_mean']:.3f}</td><td>{stats['height_std']:.3f}</td></tr>
                <tr><td>Aire</td><td>{stats['area_mean']:.3f}</td><td>{stats['area_std']:.3f}</td></tr>
            </table>
            """
        
        html += """
    </div>
</body>
</html>
"""
        
        with open(output_path, 'w', encoding='utf-8') as f:
            f.write(html)
        
        safe_print(f"📄 Rapport HTML généré: {output_path}")


def main():
    """Fonction principale pour tests"""
    import argparse
    
    parser = argparse.ArgumentParser(description="Validation de dataset YOLO")
    parser.add_argument("dataset_dir", help="Chemin vers le dossier du dataset")
    parser.add_argument("--html", action="store_true", help="Générer un rapport HTML")
    args = parser.parse_args()
    
    validator = DatasetValidator(args.dataset_dir)
    results = validator.validate()
    validator.print_report()
    
    if args.html:
        validator.save_report_html("validation_report.html")
    
    # Retourner un code d'erreur si des problèmes critiques
    if results['corrupted_images'] or results['annotations_out_of_bounds']:
        return 1
    return 0


if __name__ == "__main__":
    import sys
    sys.exit(main())
