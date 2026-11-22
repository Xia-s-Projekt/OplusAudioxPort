# Third-Party Notices, Copyrights & Licenses

## 1. Proprietary Prebuilt Binaries & Tuning Parameters (OPPO Find X9)

The following prebuilt shared libraries and acoustic tuning parameter files shipped under `prebuilt/` in this repository were extracted from **OPPO Find X9** stock firmware:

- `prebuilt/lib64/liboplus_audiox.so`
- `prebuilt/lib64/soundfx/libOplusAudioxAidl.so`
- `prebuilt/lib64/soundfx/libOplusSpatializerAidl.so`
- `prebuilt/etc/audio/audiox_param/*.bin` (47 AudioX tuning parameter binaries)
- `prebuilt/etc/spatializer/*.bin` (22 Spatializer HRIR / room model parameter binaries)

**Copyright Notice:**
Copyright © Guangdong OPPO Mobile Telecommunications Corp., Ltd. (OPPO / OPlus). All rights reserved.

These files remain the proprietary intellectual property of **OPPO / OPlus**. They are included strictly for interoperability, compatibility research, and personal custom-ROM porting on user-owned hardware. No ownership is claimed over OPPO/OPlus binaries or tuning parameters, and no commercial redistribution is implied or granted.

---

## 2. Third-Party Open-Source Prebuilt Libraries

### Alibaba MNN (`prebuilt/lib64/libMNN.so`)
- Extracted from OPPO Find X9 firmware as a runtime dependency of `liboplus_audiox.so`.
- Copyright © Alibaba Group Holding Limited.
- Licensed under the **Apache License, Version 2.0** (https://www.apache.org/licenses/LICENSE-2.0).

### LLVM `libc++_shared` (`prebuilt/lib64/libc++_shared.so`)
- Extracted from OPPO Find X9 firmware as the C++ runtime required by `libOplusAudioxAidl.so` and `libOplusSpatializerAidl.so`.
- Copyright © LLVM Project contributors.
- Licensed under the **Apache License, Version 2.0 with LLVM Exceptions** (https://llvm.org/LICENSE.txt).

### Android Open Source Project (`libs/libhidlbase.so` link stub)
- Link-time VNDK v31 stub used solely during cross-compilation of `android.hardware.audio.effect.service.oplus64`.
- Copyright © The Android Open Source Project.
- Licensed under the **Apache License, Version 2.0** (https://www.apache.org/licenses/LICENSE-2.0).

---

## 3. Original Shim Source Code & Porting Scripts

All original source code and scripts authored in this repository (`src/*`, `scripts/port_audiox.sh`, `install.sh`, `build.sh`, `tests/*`, `assets/android.hardware.audio.effect.service.oplus64.rc`):

- Licensed under the **MIT License**.
- Provided "AS IS", without warranty of any kind.
