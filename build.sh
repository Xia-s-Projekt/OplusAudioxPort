#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NDK_DIR="${NDK_DIR:-/home/ryanistr/android-ndk-r27c}"
CXX="${NDK_DIR}/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android31-clang++"
STRIP="${NDK_DIR}/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip"
OUT_DIR="${OUT_DIR:-$SCRIPT_DIR/out}"
LINK_AUDIOX_LIB="${LINK_AUDIOX_LIB:-}"

if [[ ! -x "$CXX" ]]; then
    echo "[ERR] NDK compiler not found: $CXX" >&2
    exit 1
fi

mkdir -p "$OUT_DIR"

COMMON_SO_FLAGS=(-O2 -shared -fPIC -fvisibility=hidden -static-libstdc++ -Wl,--exclude-libs,ALL -Wall -Wextra)

echo "[BUILD] 1/4 liboplus_audiox_hidl.so"
"$CXX" "${COMMON_SO_FLAGS[@]}" \
    -I"$SCRIPT_DIR/src" \
    "$SCRIPT_DIR/src/oplus_audiox_hidl.cpp" \
    -o "$OUT_DIR/liboplus_audiox_hidl.so" \
    -llog -ldl

echo "[BUILD] 2/4 liboplus_spatializer_hidl.so"
"$CXX" "${COMMON_SO_FLAGS[@]}" \
    "$SCRIPT_DIR/src/oplus_spatializer_hidl.cpp" \
    -o "$OUT_DIR/liboplus_spatializer_hidl.so" \
    -llog -ldl

echo "[BUILD] 3/4 android.hardware.audio.effect.service.oplus64"
if [[ ! -f "$SCRIPT_DIR/libs/libhidlbase.so" ]]; then
    echo "[ERR] Missing link stub: $SCRIPT_DIR/libs/libhidlbase.so" >&2
    exit 1
fi
"$CXX" -O2 -static-libstdc++ -Wl,--exclude-libs,ALL -Wall -Wextra \
    "$SCRIPT_DIR/src/audio_effect_service64.cpp" \
    -o "$OUT_DIR/android.hardware.audio.effect.service.oplus64" \
    -L"$SCRIPT_DIR/libs" -lhidlbase -llog -Wl,--allow-shlib-undefined

echo "[BUILD] 4/4 liboplus_audiox_aidl_shim.so"
AUDIOX_LINK_FLAGS=()
if [[ -z "$LINK_AUDIOX_LIB" ]]; then
    for cand in \
        "$SCRIPT_DIR/libs/libOplusAudioxAidl.so" \
        "/mnt/project/RomTools/tool/tmp/oplus/mybin/lib64/soundfx/libOplusAudioxAidl.so" \
        "/mnt/project/RomTools/ROM_WORKDIR/cos16/erofs/vendor/lib64/soundfx/libOplusAudioxAidl.so"; do
        if [[ -f "$cand" ]]; then
            LINK_AUDIOX_LIB="$cand"
            break
        fi
    done
fi
if [[ -n "$LINK_AUDIOX_LIB" && -f "$LINK_AUDIOX_LIB" ]]; then
    LINK_DIR="$(dirname "$LINK_AUDIOX_LIB")"
    AUDIOX_LINK_FLAGS=(-L"$LINK_DIR" -lOplusAudioxAidl '-Wl,-rpath,$ORIGIN' -Wl,--allow-shlib-undefined)
else
    echo "[ERR] libOplusAudioxAidl.so not found for linking AIDL shim (set LINK_AUDIOX_LIB)" >&2
    exit 1
fi

"$CXX" "${COMMON_SO_FLAGS[@]}" \
    -I"$SCRIPT_DIR/src" \
    "$SCRIPT_DIR/src/oplus_audiox_aidl_shim.cpp" \
    -o "$OUT_DIR/liboplus_audiox_aidl_shim.so" \
    "${AUDIOX_LINK_FLAGS[@]}" -llog -ldl

if [[ -x "$STRIP" ]]; then
    "$STRIP" --strip-unneeded \
        "$OUT_DIR/liboplus_audiox_hidl.so" \
        "$OUT_DIR/liboplus_spatializer_hidl.so" \
        "$OUT_DIR/liboplus_audiox_aidl_shim.so"
    "$STRIP" --strip-all "$OUT_DIR/android.hardware.audio.effect.service.oplus64"
fi

if [[ "${BUILD_TESTS:-0}" == "1" ]]; then
    echo "[BUILD] test_aidl_shim"
    "$CXX" -O2 -static-libstdc++ -Wall -Wextra \
        "$SCRIPT_DIR/tests/test_aidl_shim.cpp" \
        -o "$OUT_DIR/test_aidl_shim" \
        -ldl -lm
fi

echo "[OK] Built artifacts in $OUT_DIR:"
ls -la "$OUT_DIR"
