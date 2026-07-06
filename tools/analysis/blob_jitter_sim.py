"""Simulate the blob re-anchor jitter: why is the pose 3px-precise one-shot
but ~30px non-repeatable frame-to-frame, and what does each fix buy?

Companion to docs/BLOB_REANCHOR_JITTER_ANALYSE.md (2026-07-06).
Run: pip install numpy opencv-python-headless && python3 blob_jitter_sim.py

Model of the field scene (D405 848x480, board filling the frame, 123 comps,
26-41 matched per tick, median inlier reproj 3.3px — user log 2026-07-03):

  - N_pool matchable components spread over the frame (central margin).
  - Each component i has a FIXED bias b_i (MSER bbox center vs true body
    center: body-only vs body+shadow vs glare patch) ~ N(0, sig_bias^2).
  - Each frame adds fresh noise ~ N(0, sig_noise^2).
  - Each tick, a random subset of K components is matched (MSER finds a
    different region set every frame -> different consensus subset).

Fit per tick: 8-DOF homography (current ComponentReanchor::estimate code:
cv2.findHomography RANSAC 6px) vs 4-DOF similarity (estimateAffinePartial2D).

Metrics per tick:
  - median inlier reprojection error (what the log reports: 3.3px)
  - corner shift vs previous tick (what the 12px drift gate measures)
  - corner shift vs ground truth (one-shot accuracy at board corners)
"""
import numpy as np
import cv2

rng = np.random.default_rng(7)

W, H = 848, 480
corners = np.array([[0, 0], [W, 0], [W, H], [0, H]], dtype=np.float64)

def run(n_pool=60, k_match=35, sig_bias=2.5, sig_noise=1.0,
        resample=True, model="H", ticks=200, spread_margin=60):
    # true pose = identity (board coords == image coords, WLOG)
    pts = np.column_stack([
        rng.uniform(spread_margin, W - spread_margin, n_pool),
        rng.uniform(spread_margin, H - spread_margin, n_pool)])
    bias = rng.normal(0, sig_bias, (n_pool, 2))

    prev_c = None
    d_prev, d_true, med_reproj = [], [], []
    fixed_idx = rng.choice(n_pool, k_match, replace=False)
    for _ in range(ticks):
        idx = rng.choice(n_pool, k_match, replace=False) if resample else fixed_idx
        src = pts[idx]
        dst = src + bias[idx] + rng.normal(0, sig_noise, (k_match, 2))
        if model == "H":
            M, mask = cv2.findHomography(src.astype(np.float32),
                                         dst.astype(np.float32),
                                         cv2.RANSAC, 6.0)
            if M is None: continue
            proj = cv2.perspectiveTransform(src.reshape(-1, 1, 2), M).reshape(-1, 2)
            c = cv2.perspectiveTransform(corners.reshape(-1, 1, 2), M).reshape(-1, 2)
        else:
            A, mask = cv2.estimateAffinePartial2D(src.astype(np.float32),
                                                  dst.astype(np.float32),
                                                  method=cv2.RANSAC,
                                                  ransacReprojThreshold=6.0)
            if A is None: continue
            proj = src @ A[:, :2].T + A[:, 2]
            c = corners @ A[:, :2].T + A[:, 2]
        inl = mask.ravel().astype(bool)
        med_reproj.append(np.median(np.linalg.norm(proj[inl] - dst[inl], axis=1)))
        d_true.append(np.max(np.linalg.norm(c - corners, axis=1)))
        if prev_c is not None:
            d_prev.append(np.max(np.linalg.norm(c - prev_c, axis=1)))
        prev_c = c
    return (np.median(med_reproj),
            np.median(d_true), np.percentile(d_true, 90),
            np.median(d_prev), np.percentile(d_prev, 90))

hdr = f"{'config':<58}{'medReproj':>10}{'corner-vs-true med/p90':>24}{'tick-to-tick med/p90':>22}"
print(hdr); print("-" * len(hdr))
for label, kw in [
    ("current: 8-DOF H, subset resampled each tick",        dict(model="H", resample=True)),
    ("8-DOF H, SAME subset every tick (no MSER lottery)",   dict(model="H", resample=False)),
    ("similarity fit, subset resampled each tick",          dict(model="S", resample=True)),
    ("similarity fit, same subset",                         dict(model="S", resample=False)),
    ("8-DOF H, resampled, K=70 matches (better detector)",  dict(model="H", resample=True, k_match=70, n_pool=110)),
    ("similarity, resampled, K=70 matches",                 dict(model="S", resample=True, k_match=70, n_pool=110)),
    ("8-DOF H, resampled, no per-comp bias (centroid fix)", dict(model="H", resample=True, sig_bias=0.5, sig_noise=1.5)),
    ("similarity, resampled, no per-comp bias",             dict(model="S", resample=True, sig_bias=0.5, sig_noise=1.5)),
]:
    m, t50, t90, p50, p90 = run(**kw)
    print(f"{label:<58}{m:>9.1f}px{t50:>11.1f}/{t90:>5.1f}px{p50:>13.1f}/{p90:>5.1f}px")
