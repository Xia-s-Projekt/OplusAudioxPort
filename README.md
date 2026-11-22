# OPlusAudioxPort

Standalone source tree, NDK build system, and dynamic vendor porting toolkit for porting **OPlus AudioX (OReality)** and **OPlus Spatializer** across both **HIDL** (`IEffectsFactory`) and **AIDL** (`IFactory`) Android vendor partitions.

---

## Architecture

### Dynamic HAL Detection (`port_audiox.sh`)

`scripts/port_audiox.sh` inspects the target vendor's VINTF manifests (`etc/vintf/manifest.xml`, `etc/vintf/manifest/*.xml`, `odm/etc/vintf/**`), `bin/hw/*` ELF class & interface strings, and `etc/audio_effects*.xml` to pick the exact deployment strategy automatically:

| Detected Target HAL | Prebuilt `service.oplus64` + 32-bit HAL Patch | Deployed Effect Libraries | Config Patched |
|---|---|---|---|
| **AIDL** (`android.hardware.audio.effect.IFactory`) | **Skipped** — uses vendor's own AIDL `IFactory` | `liboplus_audiox_aidl_shim.so` (default `--aidl-mode shim`) or `libOplusAudioxAidl.so` (`--aidl-mode direct`), plus `libOplusSpatializerAidl.so` | `etc/audio_effects_config.xml` |
| **HIDL 64-bit** (`IEffectsFactory` hosted in ELF64) | **Skipped** — uses vendor's own 64-bit `IEffectsFactory` | `liboplus_audiox_hidl.so`, `liboplus_spatializer_hidl.so`, `libOplusAudioxAidl.so`, `libOplusSpatializerAidl.so` | `etc/audio_effects.xml` |
| **HIDL 32-bit** (`IEffectsFactory` hosted in ELF32, e.g. MTK) | **Deployed** — stages `android.hardware.audio.effect.service.oplus64` + `.rc` (inheriting vendor audio SELinux exec type) and disarms 32-bit `IEffectsFactory` registration + mandatory abort | `liboplus_audiox_hidl.so`, `liboplus_spatializer_hidl.so`, `libOplusAudioxAidl.so`, `libOplusSpatializerAidl.so` | `etc/audio_effects.xml` |

In all modes, the script also:
- **Auto-resolves ELF `DT_NEEDED` dependencies recursively** (e.g. `libOplusAudioxAidl.so` -> `liboplus_audiox.so` -> `libMNN.so`), automatically isolating `libc++_shared.so` as `libc++_shared_oplus.so` (`patchelf --set-soname` + `--replace-needed`) whenever the vendor ships a differing `libc++_shared.so`, and verifies zero unresolved non-system `DT_NEEDED` entries post-deployment.
- Stages all 47 `etc/audio/audiox_param/*.bin` and 22 `etc/spatializer/*.bin` parameter files plus `/odm/etc/audio/audiox_param` and `/odm/etc/spatializer` symlinks.
- Patches all `etc/audio_policy_configuration*.xml` variants with `<mixPort name="spatializer" flags="AUDIO_OUTPUT_FLAG_SPATIALIZER">` and sink routes (`Speaker`, `Wired Headset`, `Wired Headphones`, `USB Device Out`, `USB Headset Out`, `BT A2DP Out`, `BT A2DP Headphones`, `BT A2DP Speaker`).
- Injects `persist.sys.rianixia.audiox_leveler=true` into `vendor/build.prop`.
- Updates `erofs/config/vendor_file_contexts` and `erofs/config/vendor_fs_config` idempotently.

---

## Components

- `src/rynx_audiox_dsp.h` — Shared DSP engine used by both HIDL and AIDL shims:
  - **RynxModeDsp**: Differentiates OReality modes (**Smart**, **Immersive**, **Fidelity**, **Custom**), fixes OPlus's `vol=0` mode-switch bug (`0x100100be`), enables native `BINPARFIR=1` for Speaker Immersive, applies click-free 35 ms wet/dry transitions for Immersive stereo width + Haas early reflections and Fidelity 3-band clarity EQ.
  - **RynxVolumeLeveler**: Volume-slider-aware linear RMS makeup gain + transparent peak limiter (`kCeiling = 0.966f`).
- `src/oplus_audiox_hidl.cpp` — 64-bit HIDL `AELI` shim wrapping `OplusAudioxEffect` (`sizeof = 0xde38`, UUID `41f6c0f4-5d8f-11ec-bf63-0242ac130002`), with optional non-48 kHz resampling (`persist.sys.rianixia.downsample_audiox`).
- `src/oplus_spatializer_hidl.cpp` — 64-bit HIDL `AELI` shim wrapping `OplusSpatializerEffect` (`sizeof = 0x50f10`, Type `ccd4cf09-a79d-46c2-9aae-06a1698d6c8f`, UUID `de4d6aaf-4e9d-473d-8168-4ce92d50a637`), advertising 5.1 / 5.1.2 / 7.1 / 7.1.4 channel masks and syncing HIDL channel masks into `OplusSpatializerEffect`'s AIDL `Common` struct.
- `src/oplus_audiox_aidl_shim.cpp` — 64-bit AIDL wrapper library exporting `queryEffect`, `createEffect`, `destroyEffect` (`DT_RUNPATH=$ORIGIN`, `DT_NEEDED=libOplusAudioxAidl.so`) that PLT-interposes `OplusAudioxEffect::processforAidl`, `setParam`, `setDevice`, `setCommon`, `enable`, `reset`, `release`, and `EffectContext::setVolumeStereo` so AIDL vendors get `RynxVolumeLeveler` and `RynxModeDsp` while using their own AIDL `IFactory`.
- `src/audio_effect_service64.cpp` — Standalone 64-bit HIDL `IEffectsFactory` service registering `@7.0::IEffectsFactory/default` (with automatic `@6.0` fallback) against VNDK v31 `libhidlbase.so`.
- `libs/libhidlbase.so` — Link-time VNDK v31 `libhidlbase.so` stub.
- `assets/android.hardware.audio.effect.service.oplus64.rc` — Init service definition for `hidl32` split mode.
- `scripts/port_audiox.sh` — Standalone dynamic porting engine.
- `tests/test_aidl_shim.cpp` — On-device test verifying AIDL shim `$ORIGIN` loading, PLT interposition, `RynxModeDsp`, and `RynxVolumeLeveler`.

---

## Build (Arch Linux, Android NDK r27c)

Prerequisites: `android-ndk-r27c`, `patchelf`, `python3`.

```bash
NDK_DIR=/home/ryanistr/android-ndk-r27c ./build.sh
```

To also build the on-device AIDL interposition test binary:

```bash
BUILD_TESTS=1 NDK_DIR=/home/ryanistr/android-ndk-r27c ./build.sh
```

Built binaries are written to `out/`:
- `out/liboplus_audiox_hidl.so`
- `out/liboplus_spatializer_hidl.so`
- `out/liboplus_audiox_aidl_shim.so`
- `out/android.hardware.audio.effect.service.oplus64`

---

## Standalone Usage

```bash
# Auto-detect HAL mode (AIDL / HIDL64 / HIDL32) and port into an unpacked RomTools project:
./scripts/port_audiox.sh \
    --target /mnt/project/RomTools/ROM_WORKDIR/audiox/erofs \
    --assets /mnt/project/RomTools/tool/tmp/oplus/mybin

# AIDL vendor using direct libOplusAudioxAidl.so (bypassing AIDL shim):
./scripts/port_audiox.sh \
    --target /path/to/erofs \
    --assets /mnt/project/RomTools/tool/tmp/oplus/mybin \
    --aidl-mode direct

# ColorOS build stripping Dolby Atmos entries/files before applying AudioX + Spatializer:
./scripts/port_audiox.sh \
    --target /path/to/erofs \
    --assets /mnt/project/RomTools/tool/tmp/oplus/mybin \
    --strip-dolby

# Dry-run detection & plan check:
./scripts/port_audiox.sh --target /path/to/erofs --assets /mnt/project/RomTools/tool/tmp/oplus/mybin --dry-run
```

---

## Runtime Properties

| Property | Values | Effect |
|---|---|---|
| `persist.sys.rianixia.audiox_leveler` | `true` / `1` | Enables `RynxVolumeLeveler` (staged as `true` in `vendor/build.prop`) |
| `persist.sys.rianixia.audiox_leveler_log` | `true` / `1` | Enables `RynxVolumeLeveler` and `RynxModeDsp` logcat diagnostics (hidden by default) |
| `persist.sys.rianixia.downsample_audiox` | `true` / `1` | Enables 48 kHz resampling wrapper in HIDL shim for non-48 kHz streams |
