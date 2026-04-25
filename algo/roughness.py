"""Surface-roughness metrics derived from the IMU stream.

Two candidate metrics, both per-session, both computable from data the
current pipeline already produces. Goal: replace the hardcoded
per-surface coefficient table with a continuous roughness feature that
the regression uses to self-calibrate at runtime — generalizing to any
surface, not just the ones we capture.

Metric 1 — Inter-peak RMS (`inter_peak_rms_g`)
    Mask out ±half_window around each detected peak; compute RMS of
    |a|_HP_LP over the remaining samples. During the glide/recovery
    phase the cap is rolling, not impacting; the residual signal energy
    reflects surface texture transmitted through the wheels.

Metric 2 — HF/LF PSD ratio (`hf_lf_ratio`)
    Welch PSD of |a|_HP. Integrate energy in the gait band (0.5–3 Hz)
    and the surface-texture band (5–30 Hz); return their ratio.
    Amplitude-invariant since both bands scale with overall walk
    intensity. Should respond to surface texture composition rather
    than walker speed.

Both metrics work on the gravity-removed channel; the existing 0.2 Hz
HP filter handles the gravity component. The LP-smoothed channel is
what the peak detector sees, so we use that for inter-peak RMS so
"the texture between peaks" is directly comparable to peak amplitudes
in the same units.
"""

from __future__ import annotations

import numpy as np
from scipy import signal


def inter_peak_rms_g(mag_hp_lp: np.ndarray,
                     peak_sample_indices: list[int],
                     fs: float,
                     half_window_s: float = 0.2,
                     motion_mask: np.ndarray | None = None) -> float:
    """RMS of |a|_HP_LP over samples that are (a) NOT inside a peak
    window and (b) inside a motion-gate-positive window.

    Without `motion_mask`, settling-period samples (operator stationary
    before/after the walk) are included in the inter-peak set and pull
    the RMS down — confounding short walks vs long walks. Pass the
    motion gate's per-sample boolean to restrict the metric to actual
    walking time.

    Empty inter-peak-AND-motion set returns NaN.
    """
    n = len(mag_hp_lp)
    # Start with a mask of "samples in motion" (default: all).
    if motion_mask is None:
        in_motion = np.ones(n, dtype=bool)
    else:
        in_motion = np.asarray(motion_mask, dtype=bool)
        if len(in_motion) != n:
            raise ValueError(
                f"motion_mask length {len(in_motion)} != signal length {n}"
            )
    # Mask out peak windows.
    half = max(1, int(round(half_window_s * fs)))
    peak_excl = np.zeros(n, dtype=bool)
    for p in peak_sample_indices:
        i0 = max(0, p - half)
        i1 = min(n, p + half + 1)
        peak_excl[i0:i1] = True
    keep = in_motion & ~peak_excl
    inter = mag_hp_lp[keep]
    if len(inter) < 10:
        return float("nan")
    return float(np.sqrt(np.mean(inter ** 2)))


def hf_lf_ratio(mag_hp: np.ndarray,
                fs: float,
                gait_band_hz: tuple[float, float] = (0.5, 3.0),
                texture_band_hz: tuple[float, float] = (5.0, 30.0),
                motion_mask: np.ndarray | None = None) -> float:
    """Ratio of integrated PSD in the texture band to the gait band.

    Without `motion_mask`, the PSD includes settling-period samples
    where there's no gait energy; the LF band collapses toward noise
    and the ratio explodes (we saw R_hflf≈10 on stationary baselines).
    With `motion_mask`, the PSD is computed only over the contiguous
    motion segments, restoring meaning. If the motion fraction is too
    small to compute a Welch PSD reliably, returns NaN.
    """
    n = len(mag_hp)
    if motion_mask is not None:
        motion_mask = np.asarray(motion_mask, dtype=bool)
        if len(motion_mask) != n:
            raise ValueError(
                f"motion_mask length {len(motion_mask)} != signal length {n}"
            )
        x = mag_hp[motion_mask]
    else:
        x = mag_hp
    if len(x) < 50:
        return float("nan")
    nperseg = min(400, len(x))
    f, pxx = signal.welch(x, fs=fs, nperseg=nperseg, window="hann")
    gait_mask = (f >= gait_band_hz[0]) & (f <= gait_band_hz[1])
    text_mask = (f >= texture_band_hz[0]) & (f <= texture_band_hz[1])
    gait_e = float(pxx[gait_mask].sum())
    text_e = float(pxx[text_mask].sum())
    return text_e / gait_e if gait_e > 0 else float("nan")
