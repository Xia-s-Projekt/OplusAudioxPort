# OPlusAudioxPort

Standalone source tree, prebuilt OPPO Find X9 AudioX/Spatializer payloads, NDK build system, and dynamic porting installer for **OPlus AudioX (OReality)** and **OPlus Spatializer** across both **HIDL** (`IEffectsFactory`) and **AIDL** (`IFactory`) Android vendor/odm partitions.

> **Attribution & Proprietary Blobs Notice:**
> The prebuilt OPlus AudioX / Spatializer libraries (`libOplusAudioxAidl.so`, `libOplusSpatializerAidl.so`, `liboplus_audiox.so`), runtime dependencies (`libMNN.so`, `libc++_shared_oplus.so`), and all 69 acoustic tuning `.bin` files (`etc/audio/audiox_param/`, `etc/spatializer/`) shipped in `prebuilt/` are extracted from the **OPPO Find X9**. See [Copyright, Licenses & Attribution](#copyright-licenses--attribution) and [NOTICE.md](NOTICE.md).

---

## Quickstart (`install.sh` — Linux & Android / Termux)

Requirements: `bash` and `python3` only (`patchelf` is only needed if passing unpatched donor `--source-odm` binaries).

### 1. Drop-In Workdir Usage (Automatic Detection)
Extract the release zip (or clone this repository) into the same workdir as your unpacked `vendor/` (or `erofs/vendor/`, or `odm/`):

```bash
cd /path/to/your/rom_workdir
/path/to/OplusAudioxPort/install.sh
```

What `install.sh` does automatically:
1. Scans the current directory (and `erofs/`) for an unpacked `vendor/` or `odm/` partition root (preferring `vendor/` if both exist).
2. Auto-detects `config/` (`vendor_file_contexts` + `vendor_fs_config`, or `odm_file_contexts` + `odm_fs_config`).
3. If neither `vendor/` nor `odm/` is found, prompts interactively:
   ```text
   Enter folder path to scan (or 'q' to quit):
   ```
   and scans the folder you enter for `vendor/` or `odm/`.
4. Invokes `scripts/port_audiox.sh` using the repository's bundled `prebuilt/` binaries and tuning parameters.

### 2. Explicit Target or CLI Options
Any extra flags are forwarded directly to `scripts/port_audiox.sh`:

```bash
# Point directly at an unpacked erofs/, vendor/, or odm/ folder:
./install.sh /mnt/project/RomTools/ROM_WORKDIR/audiox/erofs

# AIDL vendor: route AudioX directly to libOplusAudioxAidl.so instead of the AIDL shim:
./install.sh /path/to/erofs --aidl-mode direct

# ColorOS build: strip Dolby Atmos libs/configs/XML entries before installing AudioX + Spatializer:
./install.sh /path/to/erofs --strip-dolby

# Dry-run HAL detection and dependency plan without modifying files:
./install.sh /path/to/erofs --dry-run
```

---

## Runtime System Properties (Toggles)

All custom DSP/logging toggles live under `persist.sys.rianixia.*` and can be changed at runtime (`setprop`) or baked into `vendor/build.prop` (or `odm/etc/build.prop`).

| Property | Accepted Values | Default | What It Controls |
|---|---|---|---|
| `persist.sys.rianixia.audiox_leveler` | `true` / `1` (enable), anything else or unset (disable) | Staged as `true` in `build.prop` (off in C++ if unset) | Enables **RynxVolumeLeveler** (volume-slider-aware linear RMS loudness leveling + transparent peak limiter at `0.966` ceiling, up to `+15 dB` makeup constrained by `-7.5 dBFS` headroom). |
| `persist.sys.rianixia.audiox_leveler_log` | `true` / `1` (enable), unset/`false`/`0` (hidden) | Hidden (`false`) | Enables periodic logcat diagnostics (`RynxVolumeLeveler:` and `RynxModeDsp:`) in `OplusAudioxHidlShim` / `OplusAudioxAidlShim`. Silent by default. |
| `persist.sys.rianixia.downsample_audiox` | `true` / `1` (enable), unset/`false`/`0` (disable) | Disabled (`false`) | Enables 48 kHz resampling bridge inside `liboplus_audiox_hidl.so` when the host audio mixer opens a non-48 kHz float stream (e.g. 96 kHz Hi-Res output -> 48 kHz AudioX DSP -> 96 kHz). |

Example runtime commands via `adb`:
```bash
# Enable debug telemetry in logcat:
adb shell setprop persist.sys.rianixia.audiox_leveler_log true
adb logcat -s OplusAudioxHidlShim OplusAudioxAidlShim

# Disable volume leveler on the fly (takes effect on next track/enable):
adb shell setprop persist.sys.rianixia.audiox_leveler false
```

---

## How Each Component & Function Works

### 1. Native C++ Shims & 64-bit Service (`src/`)

#### `src/rynx_audiox_dsp.h` — Shared DSP Engine (used by both HIDL & AIDL shims)
- `trackAudioxParam()` — Intercepts OPlus vendor parameter IDs:
  - `0x100100bb`: tracks active OReality profile (`0=Smart`, `1=Fidelity`, `2=Immersive`, `6=Custom`).
  - `0x100100c9`: tracks whether Custom 10-band GEQ is active (bypasses `RynxModeDsp` so Custom EQ remains untouched).
  - `0x100100c2` / `0x100100be`: tracks software volume pregain and fixes OPlus's mode-switch bug where `vol=0` resets the engine onto `lvl1.bin` by substituting `lastNonZeroVolIdx`.
- `ensureSpeakerImmersiveBrir()` — Checks `oplus_algo_spat_get_enable_ID` on Speaker Immersive mode and enables native `BINPARFIR=1` via `oplus_algo_spat_set_enable_ID` when `STUPMIX` is active.
- `applyModeDsp()` — Differentiates OReality modes with 35 ms click-free wet/dry crossfades:
  - **Smart (`0`) / Custom (`6`)**: transparent passthrough of native AudioX / Custom EQ.
  - **Immersive (`2`)**: keeps sub-bass (`<220 Hz`) mono-focused while widening mid/high stereo side energy, adding low-end warmth, and applying cross-fed Haas early reflections (`0.65 ms` / `1.45 ms`).
  - **Fidelity (`1`)**: 3-band clarity EQ (low-mid mud cut at `280/250 Hz`, vocal presence boost at `3.2/3.5 kHz`, high-shelf air at `8/9 kHz`).
- `applyVolumeLeveler()` — Volume-slider-aware linear RMS makeup gain (`kInTargetDb = -16 dB`, `kOutTargetDb = -4 dB`, `kLevelerAmount = 0.35`) paired with a fast-attack/smooth-release transparent peak limiter (`kCeiling = 0.966f`).

#### `src/oplus_audiox_hidl.cpp` (`liboplus_audiox_hidl.so`)
- Exports standard HIDL `AELI` (`audio_effect_library_t`) for UUID `41f6c0f4-5d8f-11ec-bf63-0242ac130002`.
- `loadAidlLib()` dynamically `dlopen`s `libOplusAudioxAidl.so` and resolves `OplusAudioxEffect` C++ ABI symbols (`sizeof = 0xde38`).
- `audioxCommand()` translates HIDL `EFFECT_CMD_*` calls (`INIT`, `SET_CONFIG`, `GET_CONFIG`, `RESET`, `ENABLE`, `DISABLE`, `SET_PARAM`, `GET_PARAM`, `SET_DEVICE`, `SET_VOLUME`, `OFFLOAD`) and feeds `RynxDspState`.
- `processResampledFloat()` / `audioxProcess()` handles optional non-48 kHz resampling, calls `OplusAudioxEffect::process`, then runs `applyModeDsp()` and `applyVolumeLeveler()`.

#### `src/oplus_spatializer_hidl.cpp` (`liboplus_spatializer_hidl.so`)
- Exports HIDL `AELI` for AOSP Spatializer type `ccd4cf09-a79d-46c2-9aae-06a1698d6c8f`, impl UUID `de4d6aaf-4e9d-473d-8168-4ce92d50a637`.
- `spatializerCommand()` answers AOSP HIDL Spatializer capability queries (`0, 1, 2, 3, 4, 5, 13` with exact length prefixes advertising `5.1`, `5.1.2`, `7.1`, `7.1.4` channel masks and binaural/transaural modes) and syncs HIDL `inMask`/`outMask` into `OplusSpatializerEffect`'s AIDL `Common` struct (`+0x3c`, `+0xd4`, `+0x50ecc`) before calling `setConfig`.

#### `src/oplus_audiox_aidl_shim.cpp` (`liboplus_audiox_aidl_shim.so`)
- Built with `DT_RUNPATH=$ORIGIN` and `DT_NEEDED=libOplusAudioxAidl.so` so Android's native AIDL `IFactory` loads the shim and auto-loads sibling `libOplusAudioxAidl.so`.
- Forwards `queryEffect`, `createEffect`, and `destroyEffect` to `libOplusAudioxAidl.so` (resolved via `dladdr` sibling lookup).
- PLT-interposes `OplusAudioxEffect::C1/D1/release/setCommon/setDevice/setParam/enable/reset/processforAidl` and `EffectContext::setVolumeStereo` so AIDL vendors get `RynxModeDsp` and `RynxVolumeLeveler` transparently without replacing the vendor's own AIDL `IFactory`.

#### `src/audio_effect_service64.cpp` (`android.hardware.audio.effect.service.oplus64`)
- Standalone 64-bit HIDL passthrough service used **only** on 32-bit HIDL audio HAL vendors (`hidl32`).
- Registers `android.hardware.audio.effect@7.0::IEffectsFactory/default` (falling back to `@6.0::IEffectsFactory/default` if `@7.0` is not declared) against VNDK v31 `libhidlbase.so`.

---

### 2. Porting Script Functions (`scripts/port_audiox.sh`)

| Function | Responsibility |
|---|---|
| `detect_audio_effect_hal()` | Scans VINTF manifests, `bin/hw/*` ELF32/ELF64 binaries, and `etc/audio_effects*.xml` (plus sibling `vendor/` when targeting `odm/`) to choose `aidl`, `hidl64`, or `hidl32`. |
| `resolve_and_deploy_elfs()` | Copies root libraries for the detected HAL mode, runs BFS over ELF64 `PT_DYNAMIC` `DT_NEEDED` entries (`libOplusAudioxAidl.so` -> `liboplus_audiox.so` -> `libMNN.so`), isolates `libc++_shared.so` as `libc++_shared_oplus.so` via `patchelf` on hash mismatch, and runs a post-deploy check asserting 0 unresolved non-system dependencies. |
| `detect_audio_exec_context()` | Inherits the vendor's existing audio HAL SELinux exec type (e.g. `mtk_hal_audio_exec`, fallback `hal_audio_default_exec`) for `android.hardware.audio.effect.service.oplus64`. |
| `patch_hidl32_service()` | Stages `android.hardware.audio.effect.service.oplus64.rc` and disarms 32-bit `IEffectsFactory` registration strings (`@[2-7].0` -> `@0.0`) plus the Thumb-2 mandatory abort branch (`5f ea c9 70 63 d0` -> `5f ea c9 70 00 bf`). Skipped on `aidl` and `hidl64`. |
| `deploy_params_and_symlinks()` | Copies all 47 `audiox_param/*.bin` and 22 `spatializer/*.bin` files; creates `/vendor/odm/etc/{audio/audiox_param,spatializer}` symlinks when targeting `vendor` (omitted when installing directly into `odm`). |
| `strip_dolby_from_vendor()` | Active when `--strip-dolby` is passed (ColorOS builds): deletes Dolby HAL/libs/configs and strips Dolby XML blocks and `dlb_*_listener` postprocess rules. |
| `patch_hidl_audio_effects_xml()` / `patch_aidl_audio_effects_xml()` | Updates `etc/audio_effects.xml` (HIDL) or `etc/audio_effects_config.xml` (AIDL) with AudioX and Spatializer entries and validates XML syntax via `xml.etree.ElementTree`. |
| `patch_audio_policy_configs()` | Adds `<mixPort name="spatializer" flags="AUDIO_OUTPUT_FLAG_SPATIALIZER">` and appends `,spatializer` to sink routes across all `etc/audio_policy_configuration*.xml` files. |
| `patch_build_prop()` | Idempotently injects `persist.sys.rianixia.audiox_leveler=true` into `vendor/build.prop` or `odm/etc/build.prop`. |
| `update_erofs_configs()` | Idempotently appends/updates `<part>_file_contexts` and `<part>_fs_config` for every deployed file, directory, and symlink. |

---

## Building from Source (Arch Linux, NDK r27c)

Prerequisites: `android-ndk-r27c`, `patchelf`, `python3`.

```bash
NDK_DIR=/home/ryanistr/android-ndk-r27c ./build.sh
```

To rebuild and update the bundled `prebuilt/` shims & service binary:
```bash
NDK_DIR=/home/ryanistr/android-ndk-r27c OUT_DIR=./out ./build.sh
cp -f out/liboplus_audiox_hidl.so out/liboplus_spatializer_hidl.so out/liboplus_audiox_aidl_shim.so prebuilt/lib64/soundfx/
cp -f out/android.hardware.audio.effect.service.oplus64 prebuilt/bin/hw/
```

To also build the on-device AIDL interposition test binary:
```bash
BUILD_TESTS=1 NDK_DIR=/home/ryanistr/android-ndk-r27c ./build.sh
```

---

## Copyright, Licenses & Attribution

- **OPPO / OPlus Proprietary Binaries & Tuning Parameters (OPPO Find X9):**
  All prebuilt OPlus AudioX / Spatializer binaries (`prebuilt/lib64/liboplus_audiox.so`, `prebuilt/lib64/soundfx/libOplusAudioxAidl.so`, `prebuilt/lib64/soundfx/libOplusSpatializerAidl.so`) and tuning parameter files (`prebuilt/etc/audio/audiox_param/*.bin`, `prebuilt/etc/spatializer/*.bin`) are **extracted from the OPPO Find X9** and are **Copyright © Guangdong OPPO Mobile Telecommunications Corp., Ltd. (OPPO / OPlus)**. All rights remain with OPPO / OPlus; included strictly for interoperability and custom-ROM porting research.
- **Alibaba MNN (`prebuilt/lib64/libMNN.so`):** Copyright © Alibaba Group Holding Limited — Apache License 2.0.
- **LLVM `libc++_shared` (`prebuilt/lib64/libc++_shared_oplus.so`):** Copyright © LLVM Project — Apache-2.0 with LLVM Exception.
- **AOSP (`libs/libhidlbase.so` link stub):** Copyright © The Android Open Source Project — Apache License 2.0.
- **Original Shims & Scripts (`src/*`, `scripts/*`, `install.sh`, `build.sh`, `tests/*`):** MIT License.

See [NOTICE.md](NOTICE.md) for full third-party copyright and license details.
