# OPlusAudioxPort

Port **OPlus AudioX (OReality)** and **OPlus Spatializer** to Android `vendor` or `odm` partitions. Works on both **HIDL** and **AIDL** audio HALs, on **Linux** and **Android (Termux)**.

> **Copyright Notice:**
> The prebuilt AudioX/Spatializer libraries and tuning `.bin` files in `prebuilt/` are taken from the **OPPO Find X9** and belong to **OPPO / OPlus**. See [Copyright & Licenses](#copyright--licenses) and [NOTICE.md](NOTICE.md).

---

## How to Use

Needs only `bash` and `python3` (works on Linux and Android Termux).

### 1. Extract the Release Zip Next to Your `vendor/` or `odm/`

Put the release files in the same folder as your unpacked `vendor/` (or `odm/`, or `erofs/`):

```text
your_workdir/                  <- open terminal here
├── vendor/                    <- your unpacked vendor (or odm/)
├── config/                    <- optional: vendor_file_contexts & vendor_fs_config (auto-updated)
├── install.sh                 <- run this script
├── scripts/
│   └── port_audiox.sh         <- main patcher (auto-called by install.sh)
├── prebuilt/                  <- OPPO Find X9 blobs, tuning .bin params, and our shims
│   ├── bin/hw/                <- 64-bit HIDL effect service (only used for 32-bit HIDL vendors)
│   ├── etc/audio/audiox_param/<- 47 AudioX tuning .bin files (OPPO Find X9)
│   ├── etc/spatializer/       <- 22 Spatializer tuning .bin files (OPPO Find X9)
│   ├── etc/init/              <- init .rc for the 64-bit HIDL effect service
│   └── lib64/                 <- AudioX/Spatializer .so libs, deps, and HIDL/AIDL shims
├── README.md
└── NOTICE.md
```

Or if you use RomTools (`erofs/` layout):

```text
your_workdir/                  <- open terminal here
├── erofs/
│   ├── vendor/                <- auto-detected
│   └── config/                <- auto-detected
├── install.sh                 <- run this
├── scripts/
└── prebuilt/
```

Then run:

```bash
./install.sh
```

What happens when you run `./install.sh`:
- If `vendor/` (or `erofs/vendor/`) is found -> patches `vendor/`.
- Else if `odm/` (or `erofs/odm/`) is found -> patches `odm/`.
- If neither is found -> asks you which folder to scan:
  ```text
  Enter folder path to scan (or 'q' to quit):
  ```
- Automatically checks whether your ROM uses **AIDL** or **HIDL**:
  - **AIDL vendor:** Keeps the vendor's own AIDL `IFactory` (does not install our prebuilt service) and routes AudioX through `liboplus_audiox_aidl_shim.so` so volume leveling and sound modes work.
  - **HIDL 64-bit vendor:** Keeps the vendor's own 64-bit `IEffectsFactory` and installs our HIDL shims.
  - **HIDL 32-bit vendor (e.g. MediaTek):** Installs our 64-bit `android.hardware.audio.effect.service.oplus64`, disarms the 32-bit `IEffectsFactory`, and installs our HIDL shims.

### 2. Optional Flags

```bash
# Point to a specific folder directly:
./install.sh /path/to/folder

# AIDL vendor: use libOplusAudioxAidl.so directly (skip volume leveler / mode shim):
./install.sh --aidl-mode direct

# ColorOS builds: remove Dolby Atmos files & XML entries first:
./install.sh --strip-dolby

# Preview what the script will do without changing any files:
./install.sh --dry-run
```

---

## Props to Toggle

These can be changed live with `adb shell setprop` or set in `vendor/build.prop` (`odm/etc/build.prop` for `/odm`):

| Prop | Values | Default | What It Does |
|---|---|---|---|
| `persist.sys.rianixia.audiox_leveler` | `true` / `1` = ON, `false` / `0` = OFF | Set to `true` in `build.prop` | Turns **RynxVolumeLeveler** on or off (keeps playback loud and clean with a peak limiter). |
| `persist.sys.rianixia.audiox_leveler_log` | `true` / `1` = ON, `false` / `0` = OFF | OFF (hidden) | Shows `RynxVolumeLeveler` and `RynxModeDsp` debug logs in `logcat`. |
| `persist.sys.rianixia.downsample_audiox` | `true` / `1` = ON, `false` / `0` = OFF | OFF | Resamples non-48 kHz streams (e.g. 96 kHz) to 48 kHz for AudioX and back (HIDL shim). |

Example:
```bash
# Turn on debug logs:
adb shell setprop persist.sys.rianixia.audiox_leveler_log true
adb logcat -s OplusAudioxHidlShim OplusAudioxAidlShim

# Turn off volume leveler:
adb shell setprop persist.sys.rianixia.audiox_leveler false
```

---

## What Each File / Function Does

### Source Files (`src/`)
- `src/rynx_audiox_dsp.h` — Shared audio processing used by both the HIDL and AIDL shims:
  - **RynxVolumeLeveler** (`applyVolumeLeveler`) — Volume-aware loudness boost + peak limiter so audio stays loud without clipping.
  - **RynxModeDsp** (`applyModeDsp`) — Makes **Immersive** (wider 3D stage + warmth) and **Fidelity** (clearer vocals & treble) sound clearly different from **Smart**, while leaving **Custom** EQ untouched.
  - **Mode-switch fix** (`trackAudioxParam`) — Blocks the `vol=0` reset bug when switching modes and keeps native Speaker Immersive `BINPARFIR` active (`ensureSpeakerImmersiveBrir`).
- `src/oplus_audiox_hidl.cpp` — Builds `liboplus_audiox_hidl.so` (HIDL bridge for AudioX + optional 48 kHz resampling + Rynx DSP).
- `src/oplus_spatializer_hidl.cpp` — Builds `liboplus_spatializer_hidl.so` (HIDL bridge for Spatializer with 5.1 / 5.1.2 / 7.1 / 7.1.4 channel support).
- `src/oplus_audiox_aidl_shim.cpp` — Builds `liboplus_audiox_aidl_shim.so` (AIDL wrapper that hooks `libOplusAudioxAidl.so` so AIDL vendors also get RynxVolumeLeveler and RynxModeDsp).
- `src/audio_effect_service64.cpp` — Builds `android.hardware.audio.effect.service.oplus64` (standalone 64-bit HIDL `IEffectsFactory` service for 32-bit HIDL vendors).

### Script Functions (`scripts/port_audiox.sh`)
- `detect_audio_effect_hal()` — Checks VINTF manifests, audio HAL binaries, and XMLs to detect **AIDL**, **HIDL 64-bit**, or **HIDL 32-bit**.
- `resolve_and_deploy_elfs()` — Copies the right `.so` files, automatically finds linked libraries (`libOplusAudioxAidl.so` -> `liboplus_audiox.so` -> `libMNN.so` + `libc++_shared_oplus.so`), and verifies nothing is missing.
- `patch_hidl32_service()` — Only for 32-bit HIDL vendors: installs the 64-bit effect service + `.rc` and patches the 32-bit audio service so it does not clash or crash.
- `deploy_params_and_symlinks()` — Copies the 47 AudioX + 22 Spatializer `.bin` files and adds `/odm/etc` symlinks when installing to `vendor/`.
- `patch_hidl_audio_effects_xml()` / `patch_aidl_audio_effects_xml()` — Adds AudioX and Spatializer to `audio_effects.xml` (HIDL) or `audio_effects_config.xml` (AIDL).
- `patch_audio_policy_configs()` — Adds the `spatializer` output port and routes to all `audio_policy_configuration*.xml` files.
- `patch_build_prop()` — Adds `persist.sys.rianixia.audiox_leveler=true` to `build.prop`.
- `update_erofs_configs()` — Appends SELinux contexts and permissions to `*_file_contexts` and `*_fs_config` without duplicates.

---

## Building from Source (Arch Linux, Android NDK r27c)

```bash
NDK_DIR=/home/ryanistr/android-ndk-r27c ./build.sh
```

Built files go to `out/`.

---

## Copyright & Licenses

- **OPPO / OPlus Proprietary Files (Taken from OPPO Find X9):**
  `prebuilt/lib64/liboplus_audiox.so`, `prebuilt/lib64/soundfx/libOplusAudioxAidl.so`, `prebuilt/lib64/soundfx/libOplusSpatializerAidl.so`, `prebuilt/etc/audio/audiox_param/*.bin`, and `prebuilt/etc/spatializer/*.bin` are extracted from the **OPPO Find X9** and are **Copyright © Guangdong OPPO Mobile Telecommunications Corp., Ltd. (OPPO / OPlus)**. All rights belong to OPPO / OPlus.
- **Alibaba MNN (`prebuilt/lib64/libMNN.so`):** Apache License 2.0.
- **LLVM `libc++_shared` (`prebuilt/lib64/libc++_shared_oplus.so`):** Apache-2.0 with LLVM Exception.
- **AOSP (`libs/libhidlbase.so` link stub):** Apache License 2.0.
- **Our Shims & Scripts (`src/*`, `scripts/*`, `install.sh`, `build.sh`, `tests/*`):** MIT License.

See [NOTICE.md](NOTICE.md) for full details.
