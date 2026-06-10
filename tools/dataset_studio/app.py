#!/usr/bin/env python3
"""PCB Dataset Studio — wizard Windows de préparation/entraînement du
détecteur de composants MicroscopeIBOM.

Lot 1 : étapes Projet / Import / Validation (+ générateur factice).
Lots 2-3 (à venir) : Split par session, Entraînement, Test & Déploiement Jetson.

UX inspirée de Pokemon-Dataset-Creator (lo26lo) ; modules génériques YOLO
vendorisés dans studio/vendor/.
"""

import json
import queue
import threading
import webbrowser
from pathlib import Path

import tkinter as tk
from tkinter import filedialog, messagebox, ttk

from studio.project import Project
from studio import import_manager, fake_dataset
from studio.validation import validate_project

APP_DIR = Path(__file__).resolve().parent

# --- thème sombre minimal (esprit Pokemon-Dataset-Creator) -------------------
BG = "#1e1e2e"; BG2 = "#2a2a3c"; FG = "#cdd6f4"; ACC = "#89b4fa"
OK = "#a6e3a1"; WARN = "#f9e2af"; ERR = "#f38ba8"; MUT = "#6c7086"

STEPS = [
    "0 · Projet",
    "1 · Import",
    "2 · Validation",
    "3 · Split & équilibrage",
    "4 · Entraînement",
    "5 · Test & déploiement",
]
LOT1_STEPS = {0, 1, 2}


def load_classes() -> list[str]:
    data = json.loads((APP_DIR / "config" / "pcb_classes.json").read_text(encoding="utf-8"))
    return data["classes"]


class StudioApp(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("PCB Dataset Studio — MicroscopeIBOM")
        self.geometry("1150x760")
        self.configure(bg=BG)
        self.minsize(980, 640)

        self.project = Project()
        self.classes = load_classes()
        self.log_queue: queue.Queue[str] = queue.Queue()
        self.busy = False
        self.last_summary = None

        self._build_layout()
        self.show_step(int(self.project.data.get("last_step", 0)))
        self.after(100, self._drain_log)
        self.log(f"Projet : {self.project.workdir}")
        self.log(f"{len(self.classes)} classes chargées : {', '.join(self.classes)}")

    # ------------------------------------------------------------------ layout
    def _build_layout(self):
        # Sidebar étapes
        side = tk.Frame(self, bg=BG2, width=210)
        side.pack(side="left", fill="y")
        side.pack_propagate(False)
        tk.Label(side, text="PCB Dataset\nStudio", font=("Segoe UI", 15, "bold"),
                 bg=BG2, fg=ACC, justify="left").pack(anchor="w", padx=16, pady=(18, 14))
        self.step_buttons = []
        for i, name in enumerate(STEPS):
            b = tk.Label(side, text=name, anchor="w", font=("Segoe UI", 11),
                         bg=BG2, fg=FG, padx=16, pady=7, cursor="hand2")
            b.pack(fill="x")
            b.bind("<Button-1>", lambda _e, i=i: self.show_step(i))
            self.step_buttons.append(b)
        tk.Label(side, text="Lot 1 — étapes 0-2\nLots 2-3 à venir",
                 bg=BG2, fg=MUT, font=("Segoe UI", 9), justify="left"
                 ).pack(side="bottom", anchor="w", padx=16, pady=12)

        # Zone principale + log
        main = tk.Frame(self, bg=BG)
        main.pack(side="left", fill="both", expand=True)
        self.content = tk.Frame(main, bg=BG)
        self.content.pack(fill="both", expand=True, padx=18, pady=(14, 6))

        logf = tk.Frame(main, bg=BG2)
        logf.pack(fill="x", side="bottom")
        tk.Label(logf, text="Journal", bg=BG2, fg=MUT,
                 font=("Segoe UI", 9, "bold")).pack(anchor="w", padx=10, pady=(6, 0))
        self.log_text = tk.Text(logf, height=8, bg="#181825", fg=FG,
                                relief="flat", font=("Consolas", 9), state="disabled")
        self.log_text.pack(fill="x", padx=10, pady=(2, 8))

    # ------------------------------------------------------------------ log
    def log(self, msg: str):
        self.log_queue.put(str(msg))

    def _drain_log(self):
        try:
            while True:
                msg = self.log_queue.get_nowait()
                self.log_text.configure(state="normal")
                self.log_text.insert("end", msg + "\n")
                self.log_text.see("end")
                self.log_text.configure(state="disabled")
        except queue.Empty:
            pass
        self.after(100, self._drain_log)

    def _run_bg(self, fn, done=None):
        """Exécute fn() dans un thread ; done(result) repasse sur le thread Tk."""
        if self.busy:
            self.log("⏳ Une opération est déjà en cours…")
            return
        self.busy = True

        def worker():
            try:
                result = fn()
                if done:
                    self.after(0, lambda: done(result))
            except Exception as e:  # remonte l'erreur dans le journal, pas en crash
                self.after(0, lambda: self.log(f"❌ {type(e).__name__}: {e}"))
            finally:
                self.after(0, lambda: setattr(self, "busy", False))

        threading.Thread(target=worker, daemon=True).start()

    # ------------------------------------------------------------------ steps
    def show_step(self, i: int):
        self.project.data["last_step"] = i
        self.project.save()
        for j, b in enumerate(self.step_buttons):
            b.configure(bg=ACC if j == i else BG2, fg=BG if j == i else FG)
        for w in self.content.winfo_children():
            w.destroy()
        if i == 0:
            self._step_project()
        elif i == 1:
            self._step_import()
        elif i == 2:
            self._step_validate()
        else:
            self._step_placeholder(i)

    def _title(self, text: str, sub: str = ""):
        tk.Label(self.content, text=text, font=("Segoe UI", 16, "bold"),
                 bg=BG, fg=FG).pack(anchor="w")
        if sub:
            tk.Label(self.content, text=sub, font=("Segoe UI", 10),
                     bg=BG, fg=MUT, justify="left", wraplength=860
                     ).pack(anchor="w", pady=(2, 10))

    # --- étape 0 : projet ----------------------------------------------------
    def _step_project(self):
        self._title("Projet", "Dossier de travail (dataset importé, rapports, "
                    "entraînements) et accès Jetson (utilisé au Lot 3).")
        form = tk.Frame(self.content, bg=BG)
        form.pack(anchor="w", pady=6, fill="x")

        self._entries = {}
        rows = [("Dossier de travail", "workdir", True),
                ("Jetson — IP/hôte", "jetson_host", False),
                ("Jetson — utilisateur", "jetson_user", False),
                ("Jetson — IBOM_DATA_DIR", "jetson_data_dir", False)]
        for r, (label, key, browse) in enumerate(rows):
            tk.Label(form, text=label, bg=BG, fg=FG, font=("Segoe UI", 10)
                     ).grid(row=r, column=0, sticky="w", pady=4, padx=(0, 10))
            var = tk.StringVar(value=str(self.project.data.get(key, "")))
            self._entries[key] = var
            tk.Entry(form, textvariable=var, width=58, bg=BG2, fg=FG,
                     insertbackground=FG, relief="flat"
                     ).grid(row=r, column=1, sticky="w", ipady=3)
            if browse:
                tk.Button(form, text="…", command=lambda v=var: self._browse_dir(v),
                          bg=BG2, fg=FG, relief="flat", padx=10
                          ).grid(row=r, column=2, padx=6)

        tk.Button(self.content, text="💾 Enregistrer", command=self._save_project,
                  bg=ACC, fg=BG, relief="flat", font=("Segoe UI", 10, "bold"),
                  padx=14, pady=5).pack(anchor="w", pady=10)

        sessions = self.project.sessions()
        state = (f"✅ {len(sessions)} session(s) dans le projet"
                 if sessions else "Aucune session importée pour l'instant (étape 1).")
        tk.Label(self.content, text=state, bg=BG, fg=OK if sessions else MUT,
                 font=("Segoe UI", 10)).pack(anchor="w", pady=(6, 0))

    def _browse_dir(self, var: tk.StringVar):
        d = filedialog.askdirectory(initialdir=var.get() or str(Path.home()))
        if d:
            var.set(d)

    def _save_project(self):
        for key, var in self._entries.items():
            self.project.data[key] = var.get().strip()
        self.project.save()
        self.project.ensure_dirs()
        self.log(f"💾 Projet enregistré — workdir: {self.project.workdir}")

    # --- étape 1 : import ----------------------------------------------------
    def _step_import(self):
        self._title("Import des sessions",
                    "Source = dossier local contenant une ou plusieurs sessions "
                    "(images/ + labels/), p.ex. récupéré du Jetson par scp ou clé USB. "
                    "L'import scp direct arrivera au Lot 3.")

        row = tk.Frame(self.content, bg=BG)
        row.pack(anchor="w", fill="x", pady=4)
        self.src_var = tk.StringVar()
        tk.Entry(row, textvariable=self.src_var, width=58, bg=BG2, fg=FG,
                 insertbackground=FG, relief="flat").pack(side="left", ipady=3)
        tk.Button(row, text="…", command=lambda: self._browse_dir(self.src_var),
                  bg=BG2, fg=FG, relief="flat", padx=10).pack(side="left", padx=6)
        tk.Button(row, text="Analyser", command=self._scan_source,
                  bg=ACC, fg=BG, relief="flat", padx=12).pack(side="left", padx=6)

        self.import_list = tk.Listbox(self.content, bg=BG2, fg=FG, height=8,
                                      relief="flat", selectmode="extended",
                                      font=("Consolas", 10))
        self.import_list.pack(anchor="w", fill="x", pady=8)
        tk.Button(self.content, text="📥 Importer la sélection dans le projet",
                  command=self._do_import, bg=ACC, fg=BG, relief="flat",
                  font=("Segoe UI", 10, "bold"), padx=14, pady=5).pack(anchor="w")

        ttk.Separator(self.content, orient="horizontal").pack(fill="x", pady=16)
        tk.Label(self.content, text="Pas encore de vraies données ? Génère un "
                 "dataset factice pour tester le wizard de bout en bout :",
                 bg=BG, fg=MUT, font=("Segoe UI", 10)).pack(anchor="w")
        fr = tk.Frame(self.content, bg=BG)
        fr.pack(anchor="w", pady=6)
        self.fake_sessions = tk.IntVar(value=2)
        self.fake_images = tk.IntVar(value=30)
        tk.Label(fr, text="sessions:", bg=BG, fg=FG).pack(side="left")
        tk.Spinbox(fr, from_=1, to=10, textvariable=self.fake_sessions, width=4,
                   bg=BG2, fg=FG, relief="flat").pack(side="left", padx=(4, 12))
        tk.Label(fr, text="images/session:", bg=BG, fg=FG).pack(side="left")
        tk.Spinbox(fr, from_=5, to=500, textvariable=self.fake_images, width=5,
                   bg=BG2, fg=FG, relief="flat").pack(side="left", padx=4)
        tk.Button(fr, text="🧪 Générer", command=self._gen_fake,
                  bg=BG2, fg=FG, relief="flat", padx=12).pack(side="left", padx=12)

        self._found_sessions = []

    def _scan_source(self):
        src = Path(self.src_var.get().strip())
        self._found_sessions = import_manager.scan_source(src)
        self.import_list.delete(0, "end")
        if not self._found_sessions:
            self.log(f"⚠️  Aucune session (images/ + labels/) trouvée dans {src}")
            return
        for s in self._found_sessions:
            self.import_list.insert("end", s.summary)
        self.import_list.select_set(0, "end")
        self.log(f"🔎 {len(self._found_sessions)} session(s) trouvée(s)")

    def _do_import(self):
        sel = [self._found_sessions[i] for i in self.import_list.curselection()]
        if not sel:
            self.log("⚠️  Rien à importer — analyser une source puis sélectionner.")
            return
        self.project.ensure_dirs()
        dataset_dir = self.project.dataset_dir
        self._run_bg(lambda: import_manager.import_sessions(sel, dataset_dir, self.log),
                     done=lambda done: self.log(
                         f"✅ Import terminé : {len(done)} session(s) — "
                         f"étape suivante : Validation"))

    def _gen_fake(self):
        self.project.ensure_dirs()
        n_s, n_i = self.fake_sessions.get(), self.fake_images.get()
        dataset_dir = self.project.dataset_dir
        n_cls = len(self.classes)
        self._run_bg(lambda: fake_dataset.generate(
                         dataset_dir, n_s, n_i, n_classes=n_cls, log=self.log),
                     done=lambda created: self.log(
                         f"✅ {len(created)} session(s) factice(s) générée(s) — "
                         f"étape suivante : Validation"))

    # --- étape 2 : validation -------------------------------------------------
    def _step_validate(self):
        sessions = self.project.sessions()
        self._title("Validation",
                    f"{len(sessions)} session(s) dans le projet. Vérifie images/labels "
                    "(module repris de Pokemon-Dataset-Creator), agrège les classes, "
                    "génère des rapports HTML et un aperçu visuel des bboxes.")
        if not sessions:
            tk.Label(self.content, text="Importer d'abord des sessions (étape 1).",
                     bg=BG, fg=WARN, font=("Segoe UI", 11)).pack(anchor="w", pady=8)
            return

        bar = tk.Frame(self.content, bg=BG)
        bar.pack(anchor="w", pady=4)
        tk.Button(bar, text="🔍 Lancer la validation", command=self._do_validate,
                  bg=ACC, fg=BG, relief="flat", font=("Segoe UI", 10, "bold"),
                  padx=14, pady=5).pack(side="left")
        self.btn_report = tk.Button(bar, text="📄 Ouvrir le dernier rapport HTML",
                                    command=self._open_report, state="disabled",
                                    bg=BG2, fg=FG, relief="flat", padx=12, pady=5)
        self.btn_report.pack(side="left", padx=8)
        self.btn_preview = tk.Button(bar, text="🖼️ Ouvrir l'aperçu bboxes",
                                     command=self._open_preview, state="disabled",
                                     bg=BG2, fg=FG, relief="flat", padx=12, pady=5)
        self.btn_preview.pack(side="left")

        cols = ("session", "images", "erreurs", "avertissements")
        self.val_tree = ttk.Treeview(self.content, columns=cols, show="headings",
                                     height=6)
        for c, wpx in zip(cols, (260, 90, 90, 130)):
            self.val_tree.heading(c, text=c.capitalize())
            self.val_tree.column(c, width=wpx, anchor="w")
        self.val_tree.pack(anchor="w", fill="x", pady=8)

        tk.Label(self.content, text="Répartition des classes :", bg=BG, fg=FG,
                 font=("Segoe UI", 10, "bold")).pack(anchor="w", pady=(8, 2))
        self.class_text = tk.Text(self.content, height=8, bg=BG2, fg=FG,
                                  relief="flat", font=("Consolas", 9),
                                  state="disabled")
        self.class_text.pack(anchor="w", fill="x")

        if self.last_summary:
            self._show_summary(self.last_summary)

    def _do_validate(self):
        sessions = self.project.sessions()
        reports = self.project.reports_dir
        classes = self.classes
        self._run_bg(lambda: validate_project(sessions, reports, classes, self.log),
                     done=self._show_summary)

    def _show_summary(self, summary):
        self.last_summary = summary
        self.val_tree.delete(*self.val_tree.get_children())
        for r in summary.per_session:
            self.val_tree.insert("", "end", values=(
                r["session"], r["total_images"],
                len(r["errors"]), len(r["warnings"])))

        self.class_text.configure(state="normal")
        self.class_text.delete("1.0", "end")
        total = sum(summary.class_counts.values()) or 1
        for cls_id, n in summary.class_counts.items():
            name = self.classes[cls_id] if cls_id < len(self.classes) else f"?{cls_id}"
            bar = "█" * max(1, int(40 * n / max(summary.class_counts.values())))
            self.class_text.insert(
                "end", f"{name:<16} {n:>6}  ({100 * n / total:4.1f}%)  {bar}\n")
        missing = [self.classes[i] for i in range(len(self.classes))
                   if summary.class_counts.get(i, 0) == 0]
        if missing:
            self.class_text.insert("end", f"\n⚠️ classes sans annotation : "
                                          f"{', '.join(missing)}\n")
        self.class_text.configure(state="disabled")

        if summary.report_paths:
            self.btn_report.configure(state="normal")
        if summary.preview_path:
            self.btn_preview.configure(state="normal")
        verdict = ("✅ Dataset valide" if summary.total_errors == 0
                   else f"❌ {summary.total_errors} erreur(s) — voir rapports")
        self.log(f"{verdict} · {summary.total_images} images · "
                 f"{summary.total_warnings} avertissement(s)")

    def _open_report(self):
        if self.last_summary and self.last_summary.report_paths:
            webbrowser.open(self.last_summary.report_paths[-1].as_uri())

    def _open_preview(self):
        if self.last_summary and self.last_summary.preview_path:
            webbrowser.open(self.last_summary.preview_path.as_uri())

    # --- étapes 3-5 : placeholders Lot 2/3 ------------------------------------
    def _step_placeholder(self, i: int):
        self._title(STEPS[i].split("· ")[1],
                    "Cette étape arrive au Lot 2 (split par session + entraînement) "
                    "ou au Lot 3 (test, export ONNX, déploiement Jetson). "
                    "Voir docs/DATASET_STUDIO_PLAN.md.")
        tk.Label(self.content, text="🚧 À venir", bg=BG, fg=WARN,
                 font=("Segoe UI", 14)).pack(anchor="w", pady=18)


def main():
    app = StudioApp()
    app.mainloop()


if __name__ == "__main__":
    main()
