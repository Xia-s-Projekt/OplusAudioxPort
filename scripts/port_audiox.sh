#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

TARGET=""
CONFIG_DIR=""
ASSETS_DIR=""
SOURCE_ODM=""
AIDL_MODE="shim"      # shim | direct
FORCE_HAL="auto"      # auto | aidl | hidl32 | hidl64
STRIP_DOLBY="0"
SET_LEVELER_PROP="1"
DRY_RUN="0"

usage() {
    cat <<'EOF'
Usage: port_audiox.sh --target <erofs_dir|vendor_dir> [options]

Options:
  --target <dir>         Target erofs directory (with vendor/ & config/) or vendor directory directly
  --config-dir <dir>     Explicit erofs/config directory (auto-detected if omitted)
  --assets <dir>         Directory containing prebuilt AudioX/Spatializer binaries & params
  --source-odm <dir>     Optional donor OPlus odm directory for ELF/param fallback resolution
  --aidl-mode <mode>     AIDL routing mode: shim (default, RynxVolumeLeveler+ModeDsp) or direct
  --force-hal <mode>     Override HAL detection: auto (default) | aidl | hidl32 | hidl64
  --strip-dolby          Remove Dolby Atmos entries/files (for ColorOS builds)
  --no-leveler-prop      Do not inject persist.sys.rianixia.audiox_leveler=true in build.prop
  --dry-run              Detect HAL & print plan without modifying target
  -h, --help             Show this help
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --target)          TARGET="$2"; shift 2 ;;
        --config-dir)      CONFIG_DIR="$2"; shift 2 ;;
        --assets)          ASSETS_DIR="$2"; shift 2 ;;
        --source-odm)      SOURCE_ODM="$2"; shift 2 ;;
        --aidl-mode)       AIDL_MODE="$2"; shift 2 ;;
        --force-hal)       FORCE_HAL="$2"; shift 2 ;;
        --strip-dolby)     STRIP_DOLBY="1"; shift ;;
        --no-leveler-prop) SET_LEVELER_PROP="0"; shift ;;
        --dry-run)         DRY_RUN="1"; shift ;;
        -h|--help)         usage; exit 0 ;;
        *) echo "[ERR] Unknown argument: $1" >&2; usage; exit 1 ;;
    esac
done

if [[ -z "$TARGET" ]]; then
    echo "[ERR] --target is required" >&2
    usage
    exit 1
fi

if [[ "$AIDL_MODE" != "shim" && "$AIDL_MODE" != "direct" ]]; then
    echo "[ERR] Invalid --aidl-mode: $AIDL_MODE (expected shim or direct)" >&2
    exit 1
fi

if [[ "$FORCE_HAL" != "auto" && "$FORCE_HAL" != "aidl" && "$FORCE_HAL" != "hidl32" && "$FORCE_HAL" != "hidl64" ]]; then
    echo "[ERR] Invalid --force-hal: $FORCE_HAL" >&2
    exit 1
fi

# Resolve partition dir (vendor or odm) & config dir
PART_NAME="vendor"
if [[ -d "$TARGET/etc" && ( -d "$TARGET/lib64" || -d "$TARGET/lib" || -d "$TARGET/bin" || -f "$TARGET/build.prop" || -f "$TARGET/etc/build.prop" ) ]]; then
    VENDOR_DIR="$(cd "$TARGET" && pwd)"
    [[ "$(basename "$VENDOR_DIR")" == "odm" ]] && PART_NAME="odm" || PART_NAME="vendor"
    [[ -z "$CONFIG_DIR" && -d "$VENDOR_DIR/../config" ]] && CONFIG_DIR="$(cd "$VENDOR_DIR/../config" && pwd)"
elif [[ -d "$TARGET/vendor" ]]; then
    VENDOR_DIR="$(cd "$TARGET/vendor" && pwd)"
    PART_NAME="vendor"
    [[ -z "$CONFIG_DIR" && -d "$TARGET/config" ]] && CONFIG_DIR="$(cd "$TARGET/config" && pwd)"
elif [[ -d "$TARGET/odm" ]]; then
    VENDOR_DIR="$(cd "$TARGET/odm" && pwd)"
    PART_NAME="odm"
    [[ -z "$CONFIG_DIR" && -d "$TARGET/config" ]] && CONFIG_DIR="$(cd "$TARGET/config" && pwd)"
else
    echo "[ERR] Target does not look like an unpacked erofs/, vendor/, or odm/ directory: $TARGET" >&2
    exit 1
fi

# Auto-locate assets dir if omitted
if [[ -z "$ASSETS_DIR" ]]; then
    for cand in \
        "$SCRIPT_DIR/../prebuilt" \
        "$SCRIPT_DIR/prebuilt" \
        "$SCRIPT_DIR" \
        "$SCRIPT_DIR/.." \
        "/mnt/project/RomTools/tool/tmp/oplus/mybin"; do
        if [[ -d "$cand/lib64/soundfx" || -d "$cand/out" ]]; then
            ASSETS_DIR="$(cd "$cand" && pwd)"
            break
        fi
    done
fi

if [[ -z "$ASSETS_DIR" || ! -d "$ASSETS_DIR" ]]; then
    echo "[ERR] Assets directory not found (specify --assets)" >&2
    exit 1
fi

if ! command -v python3 >/dev/null 2>&1; then
    echo "[ERR] python3 is required (Termux: 'pkg install python', Arch: 'pacman -S python', Debian/Ubuntu: 'apt install python3')" >&2
    exit 1
fi

echo "================================================================"
echo " OPlus AudioX + Spatializer Dynamic Porting Engine"
echo "----------------------------------------------------------------"
echo " Target Dir  : $VENDOR_DIR ($PART_NAME)"
echo " Config Dir  : ${CONFIG_DIR:-<none>}"
echo " Assets Dir  : $ASSETS_DIR"
echo " Source ODM  : ${SOURCE_ODM:-<none>}"
echo " AIDL Mode   : $AIDL_MODE"
echo " HAL Mode    : $FORCE_HAL"
echo " Strip Dolby : $STRIP_DOLBY"
echo "================================================================"

python3 - "$VENDOR_DIR" "$PART_NAME" "${CONFIG_DIR:-}" "$ASSETS_DIR" "${SOURCE_ODM:-}" \
          "$SCRIPT_DIR" "$AIDL_MODE" "$FORCE_HAL" "$STRIP_DOLBY" "$SET_LEVELER_PROP" "$DRY_RUN" <<'PY'
import hashlib
import os
import re
import shutil
import stat
import struct
import subprocess
import sys
import xml.etree.ElementTree as ET
from collections import deque
from pathlib import Path

(
    vendor_arg,
    part_name,
    config_arg,
    assets_arg,
    source_odm_arg,
    script_dir_arg,
    aidl_mode,
    force_hal,
    strip_dolby_s,
    set_leveler_prop_s,
    dry_run_s,
) = sys.argv[1:12]

VENDOR_DIR = Path(vendor_arg)
PART_NAME = part_name
CONFIG_DIR = Path(config_arg) if config_arg else None
ASSETS_DIR = Path(assets_arg)
SOURCE_ODM = Path(source_odm_arg) if source_odm_arg else None
SCRIPT_DIR = Path(script_dir_arg)
STRIP_DOLBY = strip_dolby_s == "1"
SET_LEVELER_PROP = set_leveler_prop_s == "1"
DRY_RUN = dry_run_s == "1"

# Standard Android Bionic / LL-NDK / VNDK system libraries resolved by runtime linker
SYSTEM_VNDK_LIBS = {
    "ld-android.so",
    "libc.so",
    "libm.so",
    "libdl.so",
    "libdl_android.so",
    "liblog.so",
    "libz.so",
    "libstdc++.so",
    "libc++.so",
    "libcutils.so",
    "libutils.so",
    "libbase.so",
    "libhidlbase.so",
    "libbinder.so",
    "libbinder_ndk.so",
    "libfmq.so",
    "libaudioutils.so",
    "libhardware.so",
    "libprocessgroup.so",
    "libvndksupport.so",
    "libnativewindow.so",
    "libandroid_net.so",
    "libcgrouprc.so",
    "libjsoncpp.so",
    "liblzma.so",
    "libpcre2.so",
    "libselinux.so",
    "libtinyxml2.so",
    "libxml2.so",
    "libion.so",
    "libdmabufheap.so",
    "libcrypto.so",
    "libssl.so",
}

SPATIALIZER_SINKS = {
    "Speaker",
    "Wired Headset",
    "Wired Headphones",
    "USB Device Out",
    "USB Headset Out",
    "BT A2DP Out",
    "BT A2DP Headphones",
    "BT A2DP Speaker",
}

DEFAULT_RC_CONTENT = """\
service vendor.audio-effect-hal-64 /vendor/bin/hw/android.hardware.audio.effect.service.oplus64
    class hal
    user audioserver
    group audio camera drmrpc inet media mediadrm net_bt net_bt_admin net_bw_acct wakelock context_hub system sdcard_rw
    capabilities BLOCK_SUSPEND SYS_NICE
    ioprio rt 4
    task_profiles ProcessCapacityHigh HighPerformance
    onrestart restart audioserver
"""


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def parse_elf_info(path: Path):
    """Parse ELF class (32/64), DT_NEEDED, DT_SONAME, and DT_RUNPATH without external deps."""
    data = path.read_bytes()
    if len(data) < 64 or data[:4] != b"\x7fELF":
        return None
    ei_class = data[4]  # 1 = ELF32, 2 = ELF64
    endian = "<" if data[5] == 1 else ">"
    info = {
        "bits": 64 if ei_class == 2 else 32,
        "needed": [],
        "soname": None,
        "runpath": [],
    }
    if ei_class != 2:
        return info

    e_phoff = struct.unpack_from(endian + "Q", data, 32)[0]
    e_phentsize, e_phnum = struct.unpack_from(endian + "HH", data, 54)

    loads = []
    dyn_off = dyn_sz = 0
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        p_type, _p_flags, p_offset, p_vaddr, _p_paddr, p_filesz, p_memsz, _p_align = struct.unpack_from(
            endian + "IIQQQQQQ", data, off
        )
        if p_type == 1:  # PT_LOAD
            loads.append((p_vaddr, p_offset, p_filesz))
        elif p_type == 2:  # PT_DYNAMIC
            dyn_off, dyn_sz = p_offset, p_filesz

    if dyn_off == 0 or dyn_sz == 0:
        return info

    def vaddr_to_off(va: int):
        for vaddr, foff, fsz in loads:
            if vaddr <= va < vaddr + fsz:
                return foff + (va - vaddr)
        return None

    needed_idx = []
    soname_idx = None
    runpath_idx = []
    strtab_va = None
    strsz = 0

    pos = dyn_off
    end = min(dyn_off + dyn_sz, len(data))
    while pos + 16 <= end:
        d_tag, d_val = struct.unpack_from(endian + "qQ", data, pos)
        pos += 16
        if d_tag == 0:  # DT_NULL
            break
        elif d_tag == 1:  # DT_NEEDED
            needed_idx.append(d_val)
        elif d_tag == 5:  # DT_STRTAB
            strtab_va = d_val
        elif d_tag == 10:  # DT_STRSZ
            strsz = d_val
        elif d_tag == 14:  # DT_SONAME
            soname_idx = d_val
        elif d_tag in (15, 29):  # DT_RPATH / DT_RUNPATH
            runpath_idx.append(d_val)

    if strtab_va is not None:
        str_off = vaddr_to_off(strtab_va)
        if str_off is not None:
            strtab = data[str_off : str_off + (strsz if strsz else len(data) - str_off)]

            def read_cstr(idx: int) -> str:
                nul = strtab.find(b"\x00", idx)
                return strtab[idx : (nul if nul != -1 else len(strtab))].decode("utf-8", "replace")

            info["needed"] = [read_cstr(i) for i in needed_idx]
            if soname_idx is not None:
                info["soname"] = read_cstr(soname_idx)
            for i in runpath_idx:
                for p in read_cstr(i).split(":"):
                    if p:
                        info["runpath"].append(p)
    return info


def detect_audio_effect_hal():
    """Dynamically detect AIDL vs HIDL (32-bit or 64-bit) IEffectsFactory in target partition."""
    aidl_manifests = []
    hidl_manifests = []

    scan_roots = [VENDOR_DIR]
    # When targeting /odm directly, also inspect sibling /vendor for VINTF & audio HAL service detection
    sibling_vendor = VENDOR_DIR.parent / "vendor"
    if PART_NAME == "odm" and sibling_vendor.is_dir():
        scan_roots.append(sibling_vendor)

    vintf_files = []
    for root in scan_roots:
        for base in [root / "etc" / "vintf", root / "odm" / "etc" / "vintf"]:
            if (base / "manifest.xml").is_file():
                vintf_files.append((root, base / "manifest.xml"))
            mdir = base / "manifest"
            if mdir.is_dir():
                for mf in sorted(mdir.glob("*.xml")):
                    vintf_files.append((root, mf))

    hal_block_re = re.compile(r'<hal\b([^>]*)>(.*?)</hal>', re.DOTALL)
    for root, vf in vintf_files:
        text = vf.read_text(encoding="utf-8", errors="ignore")
        for attrs, body in hal_block_re.findall(text):
            if "<name>android.hardware.audio.effect</name>" not in body:
                continue
            fmt_m = re.search(r'format\s*=\s*"([^"]+)"', attrs)
            fmt = fmt_m.group(1).lower() if fmt_m else ""
            if fmt == "aidl" or "IFactory" in body:
                aidl_manifests.append(str(vf.relative_to(root)))
            elif fmt == "hidl" or "IEffectsFactory" in body:
                hidl_manifests.append(str(vf.relative_to(root)))

    aidl_bins = []
    hidl32_bins = []
    hidl64_bins = []
    for root in scan_roots:
        bin_hw = root / "bin" / "hw"
        if not bin_hw.is_dir():
            continue
        for b in sorted(bin_hw.iterdir()):
            if not b.is_file() or b.is_symlink():
                continue
            if b.name == "android.hardware.audio.effect.service.oplus64":
                continue
            raw = b.read_bytes()
            if raw[:4] != b"\x7fELF":
                continue
            bits = 64 if raw[4] == 2 else 32
            if b"android.hardware.audio.effect.IFactory" in raw or (
                b"IFactory" in raw and b"android.hardware.audio.effect" in raw
            ):
                aidl_bins.append((b, bits))
            elif b"android.hardware.audio.effect@" in raw and b"IEffectsFactory" in raw:
                if bits == 32:
                    hidl32_bins.append(b)
                else:
                    hidl64_bins.append(b)

    has_aidl_xml = any((r / "etc" / "audio_effects_config.xml").is_file() for r in scan_roots)
    has_hidl_xml = any((r / "etc" / "audio_effects.xml").is_file() for r in scan_roots)

    if force_hal != "auto":
        mode = force_hal
    elif aidl_manifests or aidl_bins or (has_aidl_xml and not hidl_manifests and not hidl32_bins and not hidl64_bins):
        mode = "aidl"
    elif hidl32_bins:
        mode = "hidl32"
    elif hidl64_bins:
        mode = "hidl64"
    elif hidl_manifests or has_hidl_xml:
        mode = "hidl32"
    else:
        raise RuntimeError("Could not detect audio effect HAL (neither AIDL IFactory nor HIDL IEffectsFactory found)")

    return {
        "mode": mode,
        "aidl_manifests": aidl_manifests,
        "hidl_manifests": hidl_manifests,
        "aidl_bins": [b.name for b, _ in aidl_bins],
        "hidl32_bins": hidl32_bins,
        "hidl64_bins": hidl64_bins,
        "has_aidl_xml": has_aidl_xml,
        "has_hidl_xml": has_hidl_xml,
    }


def build_search_dirs():
    candidates = [
        ASSETS_DIR / "lib64" / "soundfx",
        ASSETS_DIR / "lib64",
        ASSETS_DIR / "bin" / "hw",
        ASSETS_DIR / "etc" / "init",
        ASSETS_DIR / "out",
        ASSETS_DIR / "assets",
        ASSETS_DIR,
        SCRIPT_DIR.parent / "out",
        SCRIPT_DIR.parent / "assets",
    ]
    if SOURCE_ODM:
        candidates.extend([
            SOURCE_ODM / "lib64" / "soundfx",
            SOURCE_ODM / "lib64",
            SOURCE_ODM / "etc",
        ])
    seen = set()
    out = []
    for d in candidates:
        if d.is_dir():
            r = d.resolve()
            if r not in seen:
                seen.add(r)
                out.append(r)
    return out


def find_donor_file(name: str, search_dirs):
    for d in search_dirs:
        p = d / name
        if p.is_file():
            return p
    return None


def find_donor_dir(rel_candidates, base_dirs):
    for base in base_dirs:
        if not base or not base.is_dir():
            continue
        for rel in rel_candidates:
            p = base / rel
            if p.is_dir() and any(p.glob("*.bin")):
                return p
    return None


def copy_file(src: Path, dst: Path, mode: int, tracked: dict, fc_type: str, fs_gid: int, fs_mode: str):
    rel = dst.relative_to(VENDOR_DIR).as_posix()
    tracked[rel] = {"fc_type": fc_type, "uid": 0, "gid": fs_gid, "mode": fs_mode, "kind": "file"}
    if DRY_RUN:
        print(f"  [DRY-RUN] copy {src.name} -> vendor/{rel}")
        return
    dst.parent.mkdir(parents=True, exist_ok=True)
    if dst.is_symlink() or dst.exists():
        dst.unlink()
    shutil.copy2(src, dst)
    os.chmod(dst, mode)


def ensure_dir_tracked(rel_dir: str, tracked: dict, fc_type: str = "vendor_configs_file"):
    parts = rel_dir.strip("/").split("/")
    # Track newly created or AudioX-owned directories cleanly
    target_path = VENDOR_DIR / rel_dir
    if not DRY_RUN:
        target_path.mkdir(parents=True, exist_ok=True)
    tracked[rel_dir] = {"fc_type": fc_type, "uid": 0, "gid": 2000, "mode": "0755", "kind": "dir"}


def ensure_symlink_tracked(rel_link: str, link_target: str, tracked: dict):
    dst = VENDOR_DIR / rel_link
    tracked[rel_link] = {"fc_type": "vendor_configs_file", "uid": 0, "gid": 2000, "mode": "0755", "kind": "symlink"}
    if DRY_RUN:
        print(f"  [DRY-RUN] symlink vendor/{rel_link} -> {link_target}")
        return
    dst.parent.mkdir(parents=True, exist_ok=True)
    if dst.is_symlink() or dst.exists():
        if dst.is_dir() and not dst.is_symlink():
            shutil.rmtree(dst)
        else:
            dst.unlink()
    os.symlink(link_target, dst)


def run_patchelf(args):
    if not shutil.which("patchelf"):
        raise RuntimeError(
            "patchelf is required to patch unpatched libc++_shared.so SONAME/DT_NEEDED "
            "(install patchelf or use pre-isolated prebuilt assets)"
        )
    subprocess.run(["patchelf"] + args, check=True)


def resolve_and_deploy_elfs(hal_info, search_dirs, tracked):
    mode = hal_info["mode"]
    print(f"\n[STEP 2] Deploying core binaries & auto-resolving ELF DT_NEEDED dependencies ({mode})...")

    # Determine root binaries to deploy
    roots = []
    # Core OPlus AIDL effect libraries (always required: directly in AIDL mode, dlopen'd in HIDL mode)
    for lib_name in ["libOplusAudioxAidl.so", "libOplusSpatializerAidl.so"]:
        src = find_donor_file(lib_name, search_dirs)
        if not src:
            raise RuntimeError(f"Missing required OPlus library in assets/source-odm: {lib_name}")
        dst = VENDOR_DIR / "lib64" / "soundfx" / lib_name
        copy_file(src, dst, 0o644, tracked, "vendor_file", 0, "0644")
        roots.append((dst, src))

    effective_aidl_lib = "libOplusAudioxAidl.so"
    if mode == "aidl":
        if aidl_mode == "shim":
            shim_src = find_donor_file("liboplus_audiox_aidl_shim.so", search_dirs)
            if shim_src:
                shim_dst = VENDOR_DIR / "lib64" / "soundfx" / "liboplus_audiox_aidl_shim.so"
                copy_file(shim_src, shim_dst, 0o644, tracked, "vendor_file", 0, "0644")
                roots.append((shim_dst, shim_src))
                effective_aidl_lib = "liboplus_audiox_aidl_shim.so"
                print("  [AIDL] Routing oplusaudiox through liboplus_audiox_aidl_shim.so (RynxVolumeLeveler + ModeDsp)")
            else:
                print("  [WARN] liboplus_audiox_aidl_shim.so not found; falling back to direct libOplusAudioxAidl.so")
        else:
            print("  [AIDL] Routing oplusaudiox directly to libOplusAudioxAidl.so (--aidl-mode=direct)")
    else:
        for shim_name in ["liboplus_audiox_hidl.so", "liboplus_spatializer_hidl.so"]:
            src = find_donor_file(shim_name, search_dirs)
            if not src:
                raise RuntimeError(f"Missing required HIDL shim: {shim_name}")
            dst = VENDOR_DIR / "lib64" / "soundfx" / shim_name
            copy_file(src, dst, 0o644, tracked, "vendor_file", 0, "0644")
            roots.append((dst, src))

    if mode == "hidl32":
        svc_src = find_donor_file("android.hardware.audio.effect.service.oplus64", search_dirs)
        if not svc_src:
            raise RuntimeError("Missing required 64-bit HIDL effect service: android.hardware.audio.effect.service.oplus64")
        svc_dst = VENDOR_DIR / "bin" / "hw" / "android.hardware.audio.effect.service.oplus64"
        exec_ctx = detect_audio_exec_context()
        copy_file(svc_src, svc_dst, 0o755, tracked, exec_ctx, 2000, "0755")
        roots.append((svc_dst, svc_src))

    # BFS recursive DT_NEEDED resolution
    queue = deque(roots)
    visited = set()
    isolated_libcxx_deployed = False
    resolved_graph = {}

    def ensure_isolated_libcxx():
        nonlocal isolated_libcxx_deployed
        dst = VENDOR_DIR / "lib64" / "libc++_shared_oplus.so"
        if isolated_libcxx_deployed and (DRY_RUN or dst.is_file()):
            return dst
        donor = find_donor_file("libc++_shared_oplus.so", search_dirs) or find_donor_file("libc++_shared.so", search_dirs)
        if not donor:
            raise RuntimeError("Missing donor libc++_shared.so / libc++_shared_oplus.so required by OPlus AudioX libs")
        copy_file(donor, dst, 0o644, tracked, "vendor_file", 0, "0644")
        if not DRY_RUN:
            el = parse_elf_info(dst)
            if not el or el.get("soname") != "libc++_shared_oplus.so":
                run_patchelf(["--set-soname", "libc++_shared_oplus.so", str(dst)])
        isolated_libcxx_deployed = True
        queue.append((dst, donor))
        print(f"  [DEPS] Isolated C++ runtime: {donor.name} -> {PART_NAME}/lib64/libc++_shared_oplus.so")
        return dst

    while queue:
        dst_path, src_path = queue.popleft()
        key = dst_path.resolve() if dst_path.exists() else dst_path
        if key in visited:
            continue
        visited.add(key)

        inspect_path = dst_path if (not DRY_RUN and dst_path.is_file()) else src_path
        info = parse_elf_info(inspect_path)
        if not info:
            continue

        dep_summary = []
        for needed in info["needed"]:
            if needed in SYSTEM_VNDK_LIBS:
                dep_summary.append(f"{needed}(sys)")
                continue

            # Special isolation check for libc++_shared.so / libc++_shared_oplus.so
            if needed in ("libc++_shared.so", "libc++_shared_oplus.so"):
                vendor_cxx = VENDOR_DIR / "lib64" / "libc++_shared.so"
                donor_cxx = find_donor_file("libc++_shared_oplus.so", search_dirs) or find_donor_file("libc++_shared.so", search_dirs)
                need_isolation = (
                    needed == "libc++_shared_oplus.so"
                    or (vendor_cxx.is_file() and donor_cxx and sha256_file(vendor_cxx) != sha256_file(donor_cxx))
                    or (not vendor_cxx.is_file() and donor_cxx is not None)
                )
                if need_isolation:
                    ensure_isolated_libcxx()
                    if needed == "libc++_shared.so" and not DRY_RUN:
                        run_patchelf(["--replace-needed", "libc++_shared.so", "libc++_shared_oplus.so", str(dst_path)])
                    dep_summary.append("libc++_shared_oplus.so(isolated)")
                    continue
                elif vendor_cxx.is_file():
                    dep_summary.append("libc++_shared.so(vendor)")
                    continue

            # Check sibling ($ORIGIN) in soundfx or vendor/lib64
            sibling = dst_path.parent / needed
            vendor_lib = VENDOR_DIR / "lib64" / needed
            if "$ORIGIN" in info["runpath"] and (sibling.is_file() or any(r[0] == sibling for r in roots)):
                dep_summary.append(f"{needed}($ORIGIN)")
                continue
            if vendor_lib.is_file():
                dep_summary.append(f"{needed}(vendor)")
                # Keep traversing & tracking if this library is part of our AudioX asset pool
                rel_v = vendor_lib.relative_to(VENDOR_DIR).as_posix()
                if rel_v in tracked or find_donor_file(needed, search_dirs) is not None:
                    tracked.setdefault(rel_v, {"fc_type": "vendor_file", "uid": 0, "gid": 0, "mode": "0644", "kind": "file"})
                    queue.append((vendor_lib, vendor_lib))
                continue

            donor = find_donor_file(needed, search_dirs)
            if not donor:
                raise RuntimeError(
                    f"Unresolved DT_NEEDED dependency '{needed}' required by '{dst_path.name}' "
                    f"(not in target vendor/lib64 and not in assets/source-odm)"
                )
            copy_file(donor, vendor_lib, 0o644, tracked, "vendor_file", 0, "0644")
            queue.append((vendor_lib, donor))
            dep_summary.append(f"{needed}(copied)")
            print(f"  [DEPS] Auto-resolved {dst_path.name} -> {needed}")

        resolved_graph[dst_path.relative_to(VENDOR_DIR).as_posix()] = dep_summary

    # Post-deployment strict ELF verification pass
    if not DRY_RUN:
        for rel in sorted(resolved_graph):
            p = VENDOR_DIR / rel
            info = parse_elf_info(p)
            for needed in info["needed"]:
                if needed in SYSTEM_VNDK_LIBS:
                    continue
                if "$ORIGIN" in info["runpath"] and (p.parent / needed).is_file():
                    continue
                if (VENDOR_DIR / "lib64" / needed).is_file():
                    continue
                raise RuntimeError(f"POST-VERIFY FAILED: {rel} still missing DT_NEEDED '{needed}'")
        print(f"  [OK] ELF DT_NEEDED verification passed ({len(resolved_graph)} binaries checked, 0 unresolved).")

    return effective_aidl_lib


def detect_audio_exec_context() -> str:
    """Inherit SELinux exec type from vendor's existing audio HAL service in vendor_file_contexts."""
    if CONFIG_DIR and (CONFIG_DIR / "vendor_file_contexts").is_file():
        fc_text = (CONFIG_DIR / "vendor_file_contexts").read_text(encoding="utf-8", errors="ignore")
        for line in fc_text.splitlines():
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.rsplit(None, 1)
            if len(parts) != 2:
                continue
            path_re, ctx = parts
            if "/vendor/bin/hw/android\\.hardware\\.audio" in path_re and "effect\\.service\\.oplus64" not in path_re:
                cparts = ctx.split(":")
                if len(cparts) >= 3 and cparts[2].endswith("_exec"):
                    return cparts[2]
    # Fallback: check vendor_sepolicy.cil for mtk_hal_audio_exec vs hal_audio_default_exec
    cil = VENDOR_DIR / "etc" / "selinux" / "vendor_sepolicy.cil"
    if cil.is_file():
        cil_text = cil.read_text(encoding="utf-8", errors="ignore")
        if "mtk_hal_audio_exec" in cil_text:
            return "mtk_hal_audio_exec"
    return "hal_audio_default_exec"


def patch_hidl32_service(hal_info, search_dirs, tracked):
    print("\n[STEP 3] Configuring 64-bit HIDL IEffectsFactory split & disarming 32-bit audio HAL...")
    rc_dst = VENDOR_DIR / "etc" / "init" / "android.hardware.audio.effect.service.oplus64.rc"
    rc_src = find_donor_file("android.hardware.audio.effect.service.oplus64.rc", search_dirs)
    tracked[rc_dst.relative_to(VENDOR_DIR).as_posix()] = {
        "fc_type": "vendor_configs_file",
        "uid": 0,
        "gid": 0,
        "mode": "0644",
        "kind": "file",
    }
    if not DRY_RUN:
        rc_dst.parent.mkdir(parents=True, exist_ok=True)
        if rc_src:
            shutil.copy2(rc_src, rc_dst)
        else:
            rc_dst.write_text(DEFAULT_RC_CONTENT, encoding="utf-8")
        os.chmod(rc_dst, 0o644)
    print(f"  [OK] Staged {rc_dst.relative_to(VENDOR_DIR)}")

    for svc_bin in hal_info["hidl32_bins"]:
        data = bytearray(svc_bin.read_bytes())
        orig = bytes(data)

        # 1. Disarm IEffectsFactory HIDL registration strings (@2.0 .. @7.0 -> @0.0)
        str_hits = 0
        for ver in (b"7", b"6", b"5", b"4", b"2"):
            old_s = b"android.hardware.audio.effect@" + ver + b".0::IEffectsFactory"
            new_s = b"android.hardware.audio.effect@0.0::IEffectsFactory"
            start = 0
            while True:
                idx = data.find(old_s, start)
                if idx == -1:
                    break
                data[idx : idx + len(old_s)] = new_s
                str_hits += 1
                start = idx + len(old_s)

        # 2. Disarm Thumb-2 mandatory abort branch (lsls.w r0, r9, #0x1f; beq -> nop)
        abort_old = bytes.fromhex("5feac97063d0")
        abort_new = bytes.fromhex("5feac97000bf")
        abort_status = "not_present"
        if abort_old in data:
            cnt = data.count(abort_old)
            if cnt != 1:
                raise RuntimeError(f"Expected unique Thumb-2 abort sequence in {svc_bin.name}, found {cnt}")
            idx = data.find(abort_old)
            data[idx : idx + len(abort_old)] = abort_new
            abort_status = f"patched@0x{idx:x}"
        elif abort_new in data:
            abort_status = "already_patched"

        rel = svc_bin.relative_to(VENDOR_DIR)
        if bytes(data) != orig:
            if not DRY_RUN:
                svc_bin.write_bytes(data)
            print(f"  [PATCH] {rel}: disarmed {str_hits} IEffectsFactory string(s), abort={abort_status}")
        else:
            print(f"  [INFO] {rel}: already disarmed (abort={abort_status})")


def deploy_params_and_symlinks(tracked):
    print("\n[STEP 4] Deploying AudioX + Spatializer parameter .bin files and /odm symlinks...")
    base_dirs = [ASSETS_DIR, SOURCE_ODM, SCRIPT_DIR.parent / "assets"]

    audiox_src = find_donor_dir(["etc/audio/audiox_param", "audiox_param"], base_dirs)
    spat_src = find_donor_dir(["etc/spatializer", "spatializer"], base_dirs)
    if not audiox_src:
        raise RuntimeError("Missing audiox_param/*.bin directory in assets or --source-odm")
    if not spat_src:
        raise RuntimeError("Missing spatializer/*.bin directory in assets or --source-odm")

    ensure_dir_tracked("etc/audio", tracked)
    ensure_dir_tracked("etc/audio/audiox_param", tracked)
    ensure_dir_tracked("etc/spatializer", tracked)

    ax_count = 0
    for f in sorted(audiox_src.glob("*.bin")):
        copy_file(f, VENDOR_DIR / "etc" / "audio" / "audiox_param" / f.name, 0o644, tracked, "vendor_configs_file", 0, "0644")
        ax_count += 1

    sp_count = 0
    for f in sorted(spat_src.glob("*.bin")):
        copy_file(f, VENDOR_DIR / "etc" / "spatializer" / f.name, 0o644, tracked, "vendor_configs_file", 0, "0644")
        sp_count += 1

    if PART_NAME == "odm":
        print(f"  [OK] Staged {ax_count} audiox_param .bin files and {sp_count} spatializer .bin files directly in /odm/etc.")
    else:
        ensure_dir_tracked("odm/etc/audio", tracked)
        ensure_symlink_tracked("odm/etc/audio/audiox_param", "/vendor/etc/audio/audiox_param", tracked)
        ensure_symlink_tracked("odm/etc/spatializer", "/vendor/etc/spatializer", tracked)
        print(f"  [OK] Staged {ax_count} audiox_param .bin files, {sp_count} spatializer .bin files, and 2 /odm symlinks.")


def strip_dolby_from_vendor():
    print("\n[STEP 4b] Stripping Dolby Atmos remnants (--strip-dolby)...")
    dolby_paths = [
        "bin/hw/vendor.dolby.hardware.dms@2.0-service",
        "etc/init/vendor.dolby.hardware.dms@2.0-service.rc",
        "etc/vintf/manifest/manifest_dolby.xml",
        "etc/dolby",
        "odm/etc/dolby",
    ]
    for rel in dolby_paths:
        p = VENDOR_DIR / rel
        if p.exists() or p.is_symlink():
            if not DRY_RUN:
                if p.is_dir() and not p.is_symlink():
                    shutil.rmtree(p)
                else:
                    p.unlink()
            print(f"  [DEL] vendor/{rel}")

    dolby_lib_re = re.compile(r"(dolby|dlb|swdap|swgamedap|swvqe|dapparamstorage)", re.IGNORECASE)
    for ldir in ["lib", "lib/soundfx", "lib64", "lib64/soundfx"]:
        d = VENDOR_DIR / ldir
        if not d.is_dir():
            continue
        for f in sorted(d.iterdir()):
            if f.is_file() or f.is_symlink():
                if dolby_lib_re.search(f.name) and "adaptor" not in f.name.lower():
                    if not DRY_RUN:
                        f.unlink()
                    print(f"  [DEL] vendor/{ldir}/{f.name}")


def strip_dolby_xml(content: str) -> str:
    content = re.sub(r"[ \t]*<!--\s*DOLBY START\s*-->.*?<!--\s*DOLBY END\s*-->\r?\n?", "", content, flags=re.DOTALL)
    for pat in [
        r'[ \t]*<library\b[^>]*path="(?:libswgamedap|libswvqe|libswdap|libdlbvol)\.so"[^>]*/>\r?\n?',
        r'[ \t]*<effect\b[^>]*library="(?:gamedap|vqe|dap|dvl)"[^>]*/>\r?\n?',
    ]:
        content = re.sub(pat, "", content)
    # Remove <postprocess> block if it only applies dlb_*_listener effects
    def maybe_drop_postprocess(m):
        blk = m.group(0)
        applies = re.findall(r'<apply\s+effect="([^"]+)"', blk)
        if applies and all(a.startswith("dlb_") for a in applies):
            return ""
        return blk
    content = re.sub(r"[ \t]*<postprocess>.*?</postprocess>\r?\n?", maybe_drop_postprocess, content, flags=re.DOTALL)
    return content


def patch_hidl_audio_effects_xml():
    xml_path = VENDOR_DIR / "etc" / "audio_effects.xml"
    if not xml_path.is_file():
        print("  [WARN] vendor/etc/audio_effects.xml not found; skipping HIDL effects XML patch.")
        return

    content = xml_path.read_text(encoding="utf-8")
    if STRIP_DOLBY:
        content = strip_dolby_xml(content)

    # Remove legacy oppo_audiox_sw_effects entries and any stale audiox/spatializer entries
    for pat in [
        r'[ \t]*<library\b[^>]*name="(?:oppo_audiox_sw_effects|oplus_audiox_sw|spatializersw)"[^>]*/>\r?\n?',
        r'[ \t]*<effect\b[^>]*name="(?:oppo_audiox_sw_effects|oplusaudiox|spatializer)"[^>]*/>\r?\n?',
    ]:
        content = re.sub(pat, "", content)

    lib_block = (
        '        <library name="oplus_audiox_sw" path="liboplus_audiox_hidl.so"/>\n'
        '        <library name="spatializersw" path="liboplus_spatializer_hidl.so"/>\n'
    )
    eff_block = (
        '        <effect name="oplusaudiox" library="oplus_audiox_sw" uuid="41f6c0f4-5d8f-11ec-bf63-0242ac130002"/>\n'
        '        <effect name="spatializer" library="spatializersw" uuid="de4d6aaf-4e9d-473d-8168-4ce92d50a637"/>\n'
    )

    if "</libraries>" not in content or "</effects>" not in content:
        raise RuntimeError(f"Malformed audio_effects.xml: missing </libraries> or </effects> in {xml_path}")

    content = re.sub(r"([ \t]*</libraries>)", lib_block + r"\1", content, count=1)
    content = re.sub(r"([ \t]*</effects>)", eff_block + r"\1", content, count=1)

    ET.fromstring(content)
    if not DRY_RUN:
        xml_path.write_text(content, encoding="utf-8")
    print("  [OK] Patched vendor/etc/audio_effects.xml (HIDL AudioX + Spatializer)")


def patch_aidl_audio_effects_xml(audiox_so_name: str):
    targets = [
        p for p in [
            VENDOR_DIR / "etc" / "audio_effects_config.xml",
            VENDOR_DIR / "odm" / "etc" / "audio_effects_config.xml",
        ] if p.is_file()
    ]
    if not targets:
        raise RuntimeError("AIDL mode selected, but no audio_effects_config.xml found in vendor/etc or vendor/odm/etc")

    for xml_path in targets:
        content = xml_path.read_text(encoding="utf-8")
        if STRIP_DOLBY:
            content = strip_dolby_xml(content)

        for pat in [
            r'[ \t]*<library\b[^>]*name="(?:oppo_audiox_sw_effects|oplus_audiox_sw|spatializersw)"[^>]*/>\r?\n?',
            r'[ \t]*<effect\b[^>]*name="(?:oppo_audiox_sw_effects|oplusaudiox|spatializer)"[^>]*/>\r?\n?',
        ]:
            content = re.sub(pat, "", content)

        lib_block = (
            f'        <library name="oplus_audiox_sw" path="{audiox_so_name}"/>\n'
            '        <library name="spatializersw" path="libOplusSpatializerAidl.so"/>\n'
        )
        eff_block = (
            '        <effect name="oplusaudiox" library="oplus_audiox_sw" '
            'uuid="41f6c0f4-5d8f-11ec-bf63-0242ac130002" type="41f6c0f4-5d8f-11ec-bf63-0242ac130002"/>\n'
            '        <effect name="spatializer" library="spatializersw" '
            'uuid="de4d6aaf-4e9d-473d-8168-4ce92d50a637"/>\n'
        )

        if "</libraries>" not in content or "</effects>" not in content:
            raise RuntimeError(f"Malformed audio_effects_config.xml: missing </libraries> or </effects> in {xml_path}")

        content = re.sub(r"([ \t]*</libraries>)", lib_block + r"\1", content, count=1)
        content = re.sub(r"([ \t]*</effects>)", eff_block + r"\1", content, count=1)

        ET.fromstring(content)
        if not DRY_RUN:
            xml_path.write_text(content, encoding="utf-8")
        print(f"  [OK] Patched {xml_path.relative_to(VENDOR_DIR)} (AIDL AudioX={audiox_so_name}, Spatializer=libOplusSpatializerAidl.so)")


def patch_audio_policy_configs():
    etc_dir = VENDOR_DIR / "etc"
    policy_files = sorted(etc_dir.glob("audio_policy_configuration*.xml"))
    if not policy_files:
        print("  [WARN] No audio_policy_configuration*.xml found in vendor/etc")
        return

    mixport_snippet = (
        '                <mixPort name="spatializer" role="source"\n'
        '                        flags="AUDIO_OUTPUT_FLAG_SPATIALIZER">\n'
        '                    <profile name="" format="AUDIO_FORMAT_PCM_32_BIT"\n'
        '                             samplingRates="48000"\n'
        '                             channelMasks="AUDIO_CHANNEL_OUT_STEREO"/>\n'
        '                </mixPort>\n'
    )

    route_re = re.compile(r'(<route\b[^>]*?type="mix"[^>]*?sink="([^"]+)"[^>]*?sources=")([^"]*)(")', re.DOTALL)
    route_re_alt = re.compile(r'(<route\b[^>]*?sink="([^"]+)"[^>]*?type="mix"[^>]*?sources=")([^"]*)(")', re.DOTALL)

    for pf in policy_files:
        orig = pf.read_text(encoding="utf-8")
        content = orig

        if "AUDIO_OUTPUT_FLAG_SPATIALIZER" not in content:
            if '<mixPort name="hdmi_mix_output"' in content:
                content = re.sub(
                    r'([ \t]*<mixPort name="hdmi_mix_output")',
                    mixport_snippet + r"\1",
                    content,
                    count=1,
                )
            elif '<mixPort ' in content and 'role="sink"' in content:
                content = re.sub(
                    r'([ \t]*<mixPort\b[^>]*role="sink")',
                    mixport_snippet + r"\1",
                    content,
                    count=1,
                )
            elif "</mixPorts>" in content:
                content = re.sub(r"([ \t]*</mixPorts>)", mixport_snippet + r"\1", content, count=1)

        def add_spatializer_source(m):
            prefix, sink, sources, suffix = m.groups()
            if sink in SPATIALIZER_SINKS:
                src_list = [s.strip() for s in sources.split(",") if s.strip()]
                if "spatializer" not in src_list:
                    sources = sources + ",spatializer" if sources else "spatializer"
            return prefix + sources + suffix

        content = route_re.sub(add_spatializer_source, content)
        content = route_re_alt.sub(add_spatializer_source, content)

        ET.fromstring(content)
        if content != orig:
            if not DRY_RUN:
                pf.write_text(content, encoding="utf-8")
            print(f"  [OK] Patched {pf.relative_to(VENDOR_DIR)} (spatializer mixPort + sink routes)")
        else:
            print(f"  [INFO] {pf.relative_to(VENDOR_DIR)}: already contains spatializer mixPort & routes")


def patch_build_prop():
    if not SET_LEVELER_PROP:
        return
    bprop_candidates = [VENDOR_DIR / "etc" / "build.prop", VENDOR_DIR / "build.prop"] if PART_NAME == "odm" else [VENDOR_DIR / "build.prop"]
    bprop = next((p for p in bprop_candidates if p.is_file()), None)
    if not bprop:
        print(f"  [WARN] {PART_NAME} build.prop not found; skipping prop injection.")
        return
    orig = bprop.read_text(encoding="utf-8", errors="ignore")
    key = "persist.sys.rianixia.audiox_leveler"
    line_val = f"{key}=true"
    if re.search(rf"^{re.escape(key)}=", orig, flags=re.MULTILINE):
        content = re.sub(rf"^{re.escape(key)}=.*$", line_val, orig, flags=re.MULTILINE)
    elif "# RIANIXIA_PORT_END" in orig:
        content = orig.replace("# RIANIXIA_PORT_END", line_val + "\n\n# RIANIXIA_PORT_END", 1)
    else:
        sep = "" if (not orig or orig.endswith("\n")) else "\n"
        content = orig + sep + line_val + "\n"
    if content != orig:
        if not DRY_RUN:
            bprop.write_text(content, encoding="utf-8")
        print(f"  [OK] Updated {PART_NAME}/{bprop.relative_to(VENDOR_DIR)} ({line_val})")
    else:
        print(f"  [INFO] {PART_NAME}/{bprop.relative_to(VENDOR_DIR)} already has {line_val}")


def update_erofs_configs(tracked):
    if not CONFIG_DIR or not CONFIG_DIR.is_dir():
        print(f"\n[STEP 7] No erofs/config directory provided; skipping {PART_NAME}_file_contexts & {PART_NAME}_fs_config.")
        return

    print(f"\n[STEP 7] Updating {CONFIG_DIR}/{PART_NAME}_file_contexts & {PART_NAME}_fs_config...")
    fc_path = CONFIG_DIR / f"{PART_NAME}_file_contexts"
    fs_path = CONFIG_DIR / f"{PART_NAME}_fs_config"

    def escape_fc(rel_path: str) -> str:
        # Match Android/RomTools file_contexts escaping (. and +)
        esc = rel_path.replace(".", r"\.").replace("+", r"\+")
        return f"/{PART_NAME}/{esc}"

    # 1. <part>_file_contexts
    if fc_path.is_file():
        lines = fc_path.read_text(encoding="utf-8", errors="ignore").splitlines()
        existing_keys = {}
        for idx, line in enumerate(lines):
            s = line.strip()
            if not s or s.startswith("#"):
                continue
            parts = s.rsplit(None, 1)
            if len(parts) == 2:
                existing_keys[parts[0]] = idx

        added = updated = 0
        for rel, meta in tracked.items():
            fc_key = escape_fc(rel)
            fc_line = f"{fc_key} u:object_r:{meta['fc_type']}:s0"
            if fc_key in existing_keys:
                idx = existing_keys[fc_key]
                if lines[idx].strip() != fc_line:
                    lines[idx] = fc_line
                    updated += 1
            else:
                lines.append(fc_line)
                existing_keys[fc_key] = len(lines) - 1
                added += 1

        if not DRY_RUN and (added or updated):
            fc_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
        print(f"  [OK] {PART_NAME}_file_contexts: +{added} added, ~{updated} updated")

    # 2. <part>_fs_config
    if fs_path.is_file():
        lines = fs_path.read_text(encoding="utf-8", errors="ignore").splitlines()
        existing_keys = {}
        for idx, line in enumerate(lines):
            s = line.strip()
            if not s or s.startswith("#"):
                continue
            parts = s.split()
            if parts:
                existing_keys[parts[0]] = idx

        added = updated = 0
        for rel, meta in tracked.items():
            fs_key = f"{PART_NAME}/{rel}"
            fs_line = f"{fs_key} {meta['uid']} {meta['gid']} {meta['mode']}"
            if fs_key in existing_keys:
                idx = existing_keys[fs_key]
                if lines[idx].strip() != fs_line:
                    lines[idx] = fs_line
                    updated += 1
            else:
                lines.append(fs_line)
                existing_keys[fs_key] = len(lines) - 1
                added += 1

        if not DRY_RUN and (added or updated):
            fs_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
        print(f"  [OK] {PART_NAME}_fs_config:     +{added} added, ~{updated} updated")


def main():
    print("[STEP 1] Detecting target vendor Audio Effect HAL architecture...")
    hal_info = detect_audio_effect_hal()
    print(f"  -> Detected HAL mode : {hal_info['mode'].upper()}")
    print(f"  -> AIDL manifests    : {hal_info['aidl_manifests'] or 'none'}")
    print(f"  -> HIDL manifests    : {hal_info['hidl_manifests'] or 'none'}")
    print(f"  -> AIDL binaries     : {hal_info['aidl_bins'] or 'none'}")
    print(f"  -> HIDL32 binaries   : {[b.name for b in hal_info['hidl32_bins']] or 'none'}")
    print(f"  -> HIDL64 binaries   : {[b.name for b in hal_info['hidl64_bins']] or 'none'}")

    if hal_info["mode"] == "aidl":
        print("  -> Action: Using vendor's own AIDL IFactory (skipping prebuilt service & binary patching)")
    elif hal_info["mode"] == "hidl64":
        print("  -> Action: Using vendor's own 64-bit HIDL IEffectsFactory (skipping prebuilt service & binary patching)")
    else:
        print("  -> Action: Deploying 64-bit HIDL IEffectsFactory service & disarming 32-bit audio HAL")

    search_dirs = build_search_dirs()
    tracked = {}

    if STRIP_DOLBY:
        strip_dolby_from_vendor()

    effective_aidl_lib = resolve_and_deploy_elfs(hal_info, search_dirs, tracked)

    if hal_info["mode"] == "hidl32":
        patch_hidl32_service(hal_info, search_dirs, tracked)
    else:
        print("\n[STEP 3] Skipping 64-bit HIDL service deployment (vendor provides native AIDL/64-bit HAL).")

    deploy_params_and_symlinks(tracked)

    print("\n[STEP 5] Patching audio effects & audio policy XML configurations...")
    if hal_info["mode"] == "aidl":
        patch_aidl_audio_effects_xml(effective_aidl_lib)
    else:
        patch_hidl_audio_effects_xml()
    patch_audio_policy_configs()

    print("\n[STEP 6] Updating vendor/build.prop...")
    patch_build_prop()

    update_erofs_configs(tracked)

    print("\n================================================================")
    print(f" [DONE] OPlus AudioX + Spatializer port completed ({hal_info['mode'].upper()})")
    print("================================================================")


if __name__ == "__main__":
    main()
PY
