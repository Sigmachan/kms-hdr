# kms-hdr roadmap — NVIDIA feature parity for COSMIC/Wayland

Goal: bring every NVIDIA feature that has **no current COSMIC/Wayland exposure**
into this stack, turning kms-hdr from an HDR injector into *the NVIDIA control
panel COSMIC lacks*. Target: RTX 5090 (Blackwell sm120), driver 575+/610-beta,
kernel 6.19-rc.

> Verdicts below are filtered for accuracy — NVIDIA's proprietary driver exposes
> **no atomic DRM colour properties** (no `CTM`/`DEGAMMA`/`GAMMA_LUT`, no
> `NV_COLOR`). Legacy `drmModeCrtcSetGamma` is the only kernel colour path; that
> is *why* the injector has an NVIDIA fallback. Everything else is env-var,
> Vulkan-layer, NVML, or compositor work.

## Leverage ranking

| # | Feature | Now? | Mechanism | Slot | Effort |
|---|---------|------|-----------|------|--------|
| 1 | **RTX-HDR-alike** (auto-HDR SDR via inverse tone-map) | yes (OSS) | libplacebo `pl_shader_inverse_tone_map` in a Vulkan layer hooking `vkQueuePresent` | new `vk-layer` + core | M |
| 2 | **NVIDIA control panel** (ULL, frame limiter, NIS sharpen, DLDSR, Smooth Motion, vibrance) | yes | env-var/profile manager + `VK_LAYER_NV_present`; sharpen via vkBasalt-style CAS/NIS layer | panel + core | S |
| 3 | **VRR / G-SYNC toggle** | partial | driver VRR works; needs `vrr_enabled` atomic + compositor adaptive-sync | injector + panel | M |
| 4 | **Per-app profiles + OC/fan** | yes | NVML (`nvml-wrapper`, already used in perfmax) + `nvidia-smi`; Coolbits for OC | new daemon + panel | M |
| 5 | **RTX Video SR / Video HDR** | yes (OSS) | `realesrgan-ncnn-vulkan` / libplacebo `pl_scale` for video surfaces; mpv-style | core/layer | M-L |
| 6 | **Digital vibrance (Wayland)** | yes | nvibrant ioctl `/dev/nvidia-modeset` (already known) | core + panel | S |
| 7 | **RTX Dynamic Vibrance-alike** | yes (OSS) | per-frame luminance histogram → adaptive saturation in the Vulkan layer | vk-layer | L |

## Highest-leverage next build

**A libplacebo-backed Vulkan present layer (`VK_LAYER_kms_hdr`)** — vkBasalt's
proven shape (intercept `vkQueuePresentKHR`, run a compute pass, re-present). It
unlocks #1 (RTX-HDR-alike inverse tone-map), #2's sharpening (CAS/NIS), and #7
(dynamic vibrance) from one component. This is the genuinely novel, portfolio-
defining piece: open-source RTX-HDR on Wayland.

## Reality check on the crew output (do NOT cargo-cult)
- ❌ `NV_COLOR` atomic DRM prop, `DRM_MODE_CRTC_BRIGHTNESS`, `vkGetDeviceQueueTimestamp`, `vkAcquireDmaBufANDROID` — **invented**, don't exist.
- ⚠️ Several env var names (`NVIDIA_NIS`, `__GL_ULTRA_LOW_LATENCY`, `NVIDIA_FRAME_LIMITER`) need verification against the actual driver README before shipping.
- ✅ Real & worth doing: libplacebo inverse-tone-map, vkQueuePresent layer hook, NVML bindings, realesrgan-ncnn-vulkan, nvibrant, `DRM_MODE_ATOMIC_TEST_ONLY` before commits, libudev hotplug, Bradford chromatic adaptation for white balance.

## Phased plan
1. **Vulkan layer skeleton** + libplacebo wiring → RTX-HDR-alike (flagship).
2. **Control-panel surface**: env/profile manager + NVML telemetry + the cheap env-var features (ULL, sharpen, DLDSR, Smooth Motion, frame limiter).
3. **VRR/G-SYNC** atomic + compositor adaptive-sync.
4. **Video SR/HDR** (ncnn/libplacebo) for video surfaces.
5. **Per-app profiles + OC/fan** via NVML.
6. **Dynamic vibrance** histogram feedback + polish.

## Injector quality upgrades (independent, verified)
- Audit `linear_to_pq` for exact ST 2084 (not an approximation).
- Bradford chromatic adaptation for white balance (replaces naive RGB gains).
- `DRM_MODE_ATOMIC_TEST_ONLY` dry-run before every real commit.
- libudev DRM monitor → auto-reapply profile on hotplug.
- ArgyllCMS colorimeter backend + ΔE2000 auto-fit in `kms-hdr-cal` (true measured calibration — beyond KDE).
