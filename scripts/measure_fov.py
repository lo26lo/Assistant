#!/usr/bin/env python3
"""
measure_fov.py — Field-of-view & scale measurement tool (dev / calibration helper)
====================================================================================
Captures frames from the microscope or the D405 RealSense, detects a checkerboard
to compute px/mm and derives the FOV in mm. Helps answer Q2 / Q3 from
docs/MICROSCOPE_PLACEMENT_PLAN.md before finalising the incremental-tracking config.

Usage
-----
  # Microscope (V4L2), 7×5 checkerboard, 5 mm squares, with prior calibration:
  python3 scripts/measure_fov.py \\
      --camera v4l2 --device 0 \\
      --calibration ~/.config/MicroscopeIBOM/calibration.yml \\
      --checkerboard 7 5 --square-size 5.0 \\
      --frames 15 --output report_microscope.json

  # RealSense D405 (uses depth-derived scale directly, no checkerboard needed):
  python3 scripts/measure_fov.py --camera realsense --output report_d405.json

Requirements
------------
  pip install opencv-python numpy pyrealsense2   # pyrealsense2 only for RealSense mode

Output (JSON + stdout summary)
-------------------------------
  {
    "camera": "v4l2",
    "device": 0,
    "resolution": [1920, 1080],
    "calibrated": true,
    "calibration_rms_px": 0.42,
    "px_per_mm": 23.4,
    "fov_width_mm": 82.1,
    "fov_height_mm": 46.2,
    "checkerboard_detections": 12,
    "notes": ["Incremental tracking recommended: FOV < 5 mm wide"],
    "config_recommendations": {
      "microscope.anchor_pixels_per_mm": 23.4,
      "microscope.reanchor_drift_px":    40
    }
  }
"""

import argparse
import json
import sys
import time
from pathlib import Path

import cv2
import numpy as np


# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------

def parse_args():
    p = argparse.ArgumentParser(
        description="Measure px/mm and FOV for microscope or RealSense camera.")
    p.add_argument("--camera", choices=["v4l2", "realsense"], default="v4l2",
                   help="Camera backend (default: v4l2)")
    p.add_argument("--device", type=int, default=0,
                   help="V4L2 device index (default: 0)")
    p.add_argument("--width",  type=int, default=1920)
    p.add_argument("--height", type=int, default=1080)
    p.add_argument("--fps",    type=int, default=30)
    p.add_argument("--calibration", type=Path, default=None,
                   help="Path to calibration.yml (OpenCV FileStorage format)")
    p.add_argument("--checkerboard", type=int, nargs=2, default=[7, 5],
                   metavar=("COLS", "ROWS"),
                   help="Number of inner corners (default: 7 5)")
    p.add_argument("--square-size", type=float, default=5.0, dest="square_size",
                   help="Square side in mm (default: 5.0)")
    p.add_argument("--frames", type=int, default=15,
                   help="Max frames to capture (default: 15)")
    p.add_argument("--show", action="store_true",
                   help="Show live preview while capturing")
    p.add_argument("--output", type=Path, default=None,
                   help="Write JSON report to this file")
    return p.parse_args()


# ---------------------------------------------------------------------------
# Camera helpers
# ---------------------------------------------------------------------------

def open_v4l2(args):
    cap = cv2.VideoCapture(args.device)
    cap.set(cv2.CAP_PROP_FRAME_WIDTH,  args.width)
    cap.set(cv2.CAP_PROP_FRAME_HEIGHT, args.height)
    cap.set(cv2.CAP_PROP_FPS,          args.fps)
    if not cap.isOpened():
        sys.exit(f"[ERROR] Cannot open V4L2 device {args.device}")
    return cap


def grab_frames_v4l2(cap, n_frames, show=False):
    frames = []
    print(f"[INFO] Capturing {n_frames} frames from V4L2 …")
    for i in range(n_frames + 5):   # warm up first 5
        ok, frame = cap.read()
        if not ok:
            continue
        if i < 5:
            continue  # discard warm-up
        frames.append(frame.copy())
        if show:
            cv2.imshow("Capture", frame)
            if cv2.waitKey(1) == 27:
                break
        print(f"  frame {len(frames)}/{n_frames}", end="\r")
        if len(frames) >= n_frames:
            break
    if show:
        cv2.destroyAllWindows()
    return frames


def grab_frames_realsense(n_frames, width=848, height=480, fps=30, show=False):
    try:
        import pyrealsense2 as rs
    except ImportError:
        sys.exit(
            "[ERROR] pyrealsense2 not available.\n"
            "  Options:\n"
            "   1. Build librealsense with -DBUILD_PYTHON_BINDINGS=true, or\n"
            "      pip install pyrealsense2  (x86 wheels only; on Jetson use the\n"
            "      bindings produced by the librealsense build).\n"
            "   2. The D405 also exposes a UVC colour node — measure it as a plain\n"
            "      camera with a checkerboard:\n"
            "        python3 scripts/measure_fov.py --camera v4l2 --device <N> \\\n"
            "            --checkerboard 7 5 --square-size 5.0\n"
            "   3. Or just use the in-app Dev → Measure FOV & Scale dialog, which\n"
            "      reads the D405 depth-derived scale live.")
    pipeline = rs.pipeline()
    cfg = rs.config()
    cfg.enable_stream(rs.stream.color, width, height, rs.format.bgr8, fps)
    cfg.enable_stream(rs.stream.depth, width, height, rs.format.z16, fps)
    pipeline.start(cfg)
    align = rs.align(rs.stream.color)
    frames = []
    depth_frames = []
    print(f"[INFO] Capturing {n_frames} frames from RealSense …")
    for i in range(n_frames + 5):
        fs = pipeline.wait_for_frames()
        aligned = align.process(fs)
        color = np.asanyarray(aligned.get_color_frame().get_data()).copy()
        depth = np.asanyarray(aligned.get_depth_frame().get_data()).copy()
        if i < 5:
            continue
        frames.append(color)
        depth_frames.append(depth)
        print(f"  frame {len(frames)}/{n_frames}", end="\r")
        if show:
            cv2.imshow("Capture", color)
            if cv2.waitKey(1) == 27:
                break
        if len(frames) >= n_frames:
            break
    pipeline.stop()
    if show:
        cv2.destroyAllWindows()
    return frames, depth_frames


# ---------------------------------------------------------------------------
# Calibration
# ---------------------------------------------------------------------------

def load_calibration(path: Path):
    """Returns (camera_matrix, dist_coeffs, rms) or (None, None, 0.0)."""
    if path is None or not path.exists():
        return None, None, 0.0
    fs = cv2.FileStorage(str(path), cv2.FILE_STORAGE_READ)
    if not fs.isOpened():
        print(f"[WARN] Cannot open calibration file: {path}")
        return None, None, 0.0
    K     = fs.getNode("camera_matrix").mat()
    dist  = fs.getNode("dist_coeffs").mat()
    rms   = float(fs.getNode("rms_error").real())
    fs.release()
    if K is None or K.size == 0:
        return None, None, 0.0
    print(f"[INFO] Calibration loaded from {path}  (RMS {rms:.3f} px)")
    return K, dist, rms


# ---------------------------------------------------------------------------
# Checkerboard detection & px/mm
# ---------------------------------------------------------------------------

def detect_checkerboard(frames, cols, rows, square_size_mm,
                         K=None, dist=None, show_result=False):
    """
    Returns list of (px_per_mm, frame_annotated) for each successful detection.
    Uses the OpenCV solvePnP approach when calibration is available; otherwise
    falls back to measuring the average pixel distance between adjacent corners
    on the real-world grid.
    """
    pattern = (cols, rows)
    crit = (cv2.TERM_CRITERIA_EPS + cv2.TERM_CRITERIA_MAX_ITER, 30, 0.001)
    results = []

    for idx, frame in enumerate(frames):
        gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
        ok, corners = cv2.findChessboardCorners(gray, pattern,
            cv2.CALIB_CB_ADAPTIVE_THRESH | cv2.CALIB_CB_NORMALIZE_IMAGE)
        if not ok:
            continue

        corners_sub = cv2.cornerSubPix(gray, corners, (11, 11), (-1, -1), crit)

        # Simple approach: average distance between adjacent corners in px,
        # divide by square_size_mm → px/mm
        # (valid even without calibration; calibration mainly removes distortion)
        if dist is not None:
            undistorted = cv2.undistortPoints(corners_sub, K, dist, P=K)
            pts = undistorted.reshape(-1, 2)
        else:
            pts = corners_sub.reshape(-1, 2)

        # Horizontal neighbours: pts[j] and pts[j+1] within same row
        dists_h = []
        for r in range(rows):
            for c in range(cols - 1):
                i0 = r * cols + c
                i1 = i0 + 1
                d = float(np.linalg.norm(pts[i1] - pts[i0]))
                dists_h.append(d)

        # Vertical neighbours
        dists_v = []
        for r in range(rows - 1):
            for c in range(cols):
                i0 = r * cols + c
                i1 = i0 + cols
                d = float(np.linalg.norm(pts[i1] - pts[i0]))
                dists_v.append(d)

        mean_px = float(np.mean(dists_h + dists_v))
        px_per_mm = mean_px / square_size_mm

        vis = frame.copy()
        cv2.drawChessboardCorners(vis, pattern, corners_sub, ok)
        cv2.putText(vis,
                    f"{px_per_mm:.2f} px/mm",
                    (20, 40), cv2.FONT_HERSHEY_SIMPLEX, 1.2, (0, 255, 0), 2)
        results.append((px_per_mm, vis))
        print(f"  frame {idx+1}: {px_per_mm:.2f} px/mm")

        if show_result:
            cv2.imshow("Checkerboard", vis)
            cv2.waitKey(600)

    if show_result:
        cv2.destroyAllWindows()

    return results


# ---------------------------------------------------------------------------
# RealSense depth-based scale
# ---------------------------------------------------------------------------

def realsense_scale_from_depth(depth_frames, color_frames, width, height):
    """
    Estimates px/mm using the median depth of the central patch (which is the
    PCB surface at known working distance) + RealSense D405 horizontal FOV 87°.
    Approximate but gives a ballpark without a checkerboard.

    px/mm = image_width / fov_width_mm
    fov_width_mm = 2 * depth_m * tan(hfov/2) * 1000
    hfov_rad = 87° for D405 colour camera at 848×480
    """
    D405_HFOV_DEG = 87.0   # horizontal FOV of the D405 colour stream
    D405_DEPTH_SCALE = 0.0001  # default depth unit: 0.1 mm = 0.0001 m

    medians = []
    for df in depth_frames:
        cx, cy = width // 2, height // 2
        patch = df[cy-40:cy+40, cx-40:cx+40].astype(np.float32)
        nonzero = patch[patch > 0]
        if nonzero.size > 100:
            medians.append(float(np.median(nonzero)) * D405_DEPTH_SCALE)

    if not medians:
        return None, "depth patch empty — card too close/far or holes"

    depth_m = float(np.median(medians))
    import math
    hfov_rad = math.radians(D405_HFOV_DEG)
    fov_width_m = 2.0 * depth_m * math.tan(hfov_rad / 2.0)
    fov_width_mm = fov_width_m * 1000.0
    px_per_mm = width / fov_width_mm
    note = f"depth-derived (median working distance {depth_m*100:.1f} cm, HFOV={D405_HFOV_DEG}°)"
    return px_per_mm, note


# ---------------------------------------------------------------------------
# Report
# ---------------------------------------------------------------------------

def build_report(args, px_per_mm_samples, resolution, K, rms,
                 source_note="checkerboard", extra=None):
    w, h = resolution
    notes = []

    if not px_per_mm_samples:
        px_per_mm = None
        fov_w = fov_h = None
        notes.append("No successful checkerboard detections — try a cleaner print or closer distance.")
    else:
        px_per_mm = float(np.median(px_per_mm_samples))
        fov_w = w / px_per_mm
        fov_h = h / px_per_mm

        if len(px_per_mm_samples) >= 3:
            spread_pct = (max(px_per_mm_samples) - min(px_per_mm_samples)) / px_per_mm * 100
            if spread_pct > 5:
                notes.append(
                    f"Scale spread {spread_pct:.1f}% across frames — "
                    "possible zoom variation or perspective.")

        if fov_w is not None and fov_w < 5.0:
            notes.append(
                "FOV < 5 mm wide: very few iBOM features expected. "
                "Incremental tracking (Settings → Tracking → Incremental) strongly recommended.")
        elif fov_w is not None and fov_w < 15.0:
            notes.append(
                "FOV 5–15 mm: global ORB matching may struggle with repeated patterns. "
                "Incremental tracking recommended for 0201 work.")

    reco = {}
    if px_per_mm is not None:
        reco["microscope.anchor_pixels_per_mm"] = round(px_per_mm, 2)
        drift = max(5.0, fov_w * 0.1 * px_per_mm) if fov_w else 40.0
        reco["microscope.reanchor_drift_px"] = round(drift, 1)
        if fov_w is not None and fov_w < 15.0:
            reco["microscope.incremental"] = True

    report = {
        "timestamp": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "camera":  args.camera,
        "device":  args.device if args.camera == "v4l2" else "realsense",
        "resolution": list(resolution),
        "calibrated": K is not None,
        "calibration_rms_px": round(rms, 4) if K is not None else None,
        "scale_source": source_note,
        "px_per_mm":   round(px_per_mm, 4) if px_per_mm else None,
        "fov_width_mm":  round(fov_w, 2) if fov_w else None,
        "fov_height_mm": round(fov_h, 2) if fov_h else None,
        "checkerboard_detections": len(px_per_mm_samples),
        "px_per_mm_samples": [round(v, 3) for v in px_per_mm_samples],
        "notes": notes,
        "config_recommendations": reco,
    }
    if extra:
        report.update(extra)
    return report


def print_summary(report):
    sep = "─" * 60
    print()
    print(sep)
    print(f"  Camera:       {report['camera']}  {report['resolution'][0]}×{report['resolution'][1]}")
    print(f"  Calibrated:   {report['calibrated']}" +
          (f"  (RMS {report['calibration_rms_px']:.3f} px)" if report['calibrated'] else ""))
    print(f"  Scale:        {report['px_per_mm']} px/mm  [{report['scale_source']}]"
          if report['px_per_mm'] else "  Scale:        unknown")
    if report['fov_width_mm']:
        print(f"  FOV:          {report['fov_width_mm']:.1f} × {report['fov_height_mm']:.1f} mm")
    if report['notes']:
        for n in report['notes']:
            print(f"  ⚠  {n}")
    if report['config_recommendations']:
        print()
        print("  Config recommendations:")
        for k, v in report['config_recommendations'].items():
            print(f"    {k} = {v}")
    print(sep)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    args = parse_args()
    K, dist, rms = load_calibration(args.calibration)

    if args.camera == "v4l2":
        cap = open_v4l2(args)
        w = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
        h = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
        frames = grab_frames_v4l2(cap, args.frames, show=args.show)
        cap.release()

        detections = detect_checkerboard(
            frames, args.checkerboard[0], args.checkerboard[1],
            args.square_size, K=K, dist=dist, show_result=args.show)
        samples = [d[0] for d in detections]
        report = build_report(args, samples, (w, h), K, rms, "checkerboard")

    else:  # realsense
        frames, depth_frames = grab_frames_realsense(
            args.frames, args.width, args.height, args.fps, show=args.show)
        w, h = args.width, args.height

        # Try checkerboard first
        detections = detect_checkerboard(
            frames, args.checkerboard[0], args.checkerboard[1],
            args.square_size, K=K, dist=dist, show_result=args.show)
        samples = [d[0] for d in detections]

        if samples:
            report = build_report(args, samples, (w, h), K, rms, "checkerboard")
        else:
            # Fall back to depth-based estimate
            print("[INFO] No checkerboard detected — falling back to depth-based estimate.")
            px_per_mm, note = realsense_scale_from_depth(depth_frames, frames, w, h)
            samples2 = [px_per_mm] if px_per_mm else []
            report = build_report(args, samples2, (w, h), K, rms,
                                  f"depth-estimate ({note})",
                                  extra={"depth_fallback": True})

    print_summary(report)

    if args.output:
        args.output.write_text(json.dumps(report, indent=2, ensure_ascii=False))
        print(f"\n[INFO] Report saved → {args.output}")


if __name__ == "__main__":
    main()
