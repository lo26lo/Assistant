"""Check GPU avant entraînement — spécial Blackwell (RTX 50xx).

La RTX 5070 Ti est en architecture Blackwell (compute capability 12.0,
sm_120) : seuls PyTorch ≥ 2.7 compilés en CUDA 12.8 embarquent les kernels
sm_120. Un torch plus ancien voit le GPU mais plante à la première opération
("no kernel image is available"). On le détecte AVANT de lancer 300 epochs.
"""

from dataclasses import dataclass, field

INSTALL_CMDS = [
    "pip install torch torchvision --index-url https://download.pytorch.org/whl/cu128",
    "pip install ultralytics",
]


@dataclass
class GpuStatus:
    ok: bool = False                 # prêt pour l'entraînement GPU
    device: str = "cpu"              # device à passer à YOLO ("0" ou "cpu")
    summary: str = ""                # une ligne pour la GUI
    details: list[str] = field(default_factory=list)


def check_gpu() -> GpuStatus:
    st = GpuStatus()

    try:
        import torch
    except ImportError:
        st.summary = "❌ PyTorch non installé — entraînement impossible"
        st.details = ["Lancer install_training.bat, ou dans le venv :",
                      *[f"  {c}" for c in INSTALL_CMDS]]
        return st

    st.details.append(f"torch {torch.__version__}")

    if not torch.cuda.is_available():
        st.summary = "⚠️ PyTorch sans CUDA — entraînement CPU uniquement (très lent)"
        st.details += ["torch.cuda.is_available() = False",
                       "Réinstaller la roue CUDA 12.8 :",
                       *[f"  {c}" for c in INSTALL_CMDS]]
        st.device = "cpu"
        return st

    name = torch.cuda.get_device_name(0)
    cap = torch.cuda.get_device_capability(0)
    st.details.append(f"GPU: {name} (sm_{cap[0]}{cap[1]})")

    # Blackwell (sm_120) : vérifier que cette roue torch embarque le kernel
    try:
        arch_list = torch.cuda.get_arch_list()
        st.details.append(f"archs torch: {', '.join(arch_list)}")
        needed = f"sm_{cap[0]}{cap[1]}"
        if needed not in arch_list and cap >= (10, 0):
            st.summary = (f"❌ {name} ({needed}) non supporté par cette roue "
                          f"torch — installer la roue CUDA 12.8 (≥ torch 2.7)")
            st.details += ["", *[f"  {c}" for c in INSTALL_CMDS]]
            return st
    except Exception:
        pass  # get_arch_list absent sur de vieux torch : on tente quand même

    try:
        import ultralytics
        st.details.append(f"ultralytics {ultralytics.__version__}")
    except ImportError:
        st.summary = f"⚠️ GPU {name} OK mais ultralytics manquant"
        st.details += ["  pip install ultralytics"]
        return st

    vram_gb = torch.cuda.get_device_properties(0).total_memory / (1 << 30)
    st.ok = True
    st.device = "0"
    st.summary = f"✅ {name} — {vram_gb:.0f} Go VRAM, prêt pour l'entraînement"
    return st


if __name__ == "__main__":
    status = check_gpu()
    print(status.summary)
    for detail in status.details:
        print(f"  {detail}")
