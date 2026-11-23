#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK_DIR="$(pwd)"

if ! command -v python3 >/dev/null 2>&1; then
    echo "[ERR] python3 is required (Termux: 'pkg install python', Arch: 'pacman -S python', Debian/Ubuntu: 'apt install python3')" >&2
    exit 1
fi

# Locate port_audiox.sh and prebuilt assets
PORT_SCRIPT=""
ASSETS_DIR=""
for cand in "$SCRIPT_DIR/scripts/port_audiox.sh" "$SCRIPT_DIR/port_audiox.sh" "/mnt/project/RomTools/tool/tmp/oplus/port_audiox.sh"; do
    if [[ -f "$cand" ]]; then
        PORT_SCRIPT="$cand"
        break
    fi
done

for cand in "$SCRIPT_DIR/prebuilt" "$SCRIPT_DIR" "/mnt/project/RomTools/tool/tmp/oplus/myvendor"; do
    if [[ -d "$cand/lib64/soundfx" && -d "$cand/etc/audio/audiox_param" ]]; then
        ASSETS_DIR="$cand"
        break
    fi
done

if [[ -z "$PORT_SCRIPT" ]]; then
    echo "[ERR] Could not locate scripts/port_audiox.sh" >&2
    exit 1
fi
if [[ -z "$ASSETS_DIR" ]]; then
    echo "[ERR] Could not locate prebuilt AudioX assets directory" >&2
    exit 1
fi

is_partition_root() {
    local d="$1"
    [[ -d "$d" ]] || return 1
    [[ -d "$d/etc" ]] && [[ -d "$d/lib64" || -d "$d/lib" || -d "$d/bin" || -f "$d/build.prop" || -f "$d/etc/build.prop" ]]
}

CANDIDATES=()
scan_for_targets() {
    local base="$1"
    CANDIDATES=()

    [[ -d "$base" ]] || return 0
    local abs_base
    abs_base="$(cd "$base" && pwd)"

    local bname
    bname="$(basename "$abs_base")"
    if [[ "$bname" == "vendor" || "$bname" == "odm" ]] && is_partition_root "$abs_base"; then
        CANDIDATES+=("$abs_base")
        return 0
    fi

    local cand
    for cand in \
        "$abs_base/vendor" \
        "$abs_base/erofs/vendor" \
        "$abs_base/odm" \
        "$abs_base/erofs/odm"; do
        if is_partition_root "$cand"; then
            CANDIDATES+=("$(cd "$cand" && pwd)")
        fi
    done

    # Portable shallow scan (depth <= 3) via python3 so Android/Termux/Linux need no extra tools
    if [[ ${#CANDIDATES[@]} -eq 0 ]]; then
        local found_dir
        while IFS= read -r found_dir; do
            [[ -n "$found_dir" ]] && CANDIDATES+=("$found_dir")
        done < <(python3 - "$abs_base" <<'PY'
import os, sys
base = os.path.abspath(sys.argv[1])
base_depth = base.rstrip(os.sep).count(os.sep)
found = []
for root, dirs, _files in os.walk(base):
    depth = root.rstrip(os.sep).count(os.sep) - base_depth
    if depth >= 3:
        dirs[:] = []
        continue
    dirs[:] = sorted(d for d in dirs if not d.startswith("."))
    for d in list(dirs):
        if d in ("vendor", "odm"):
            p = os.path.join(root, d)
            if os.path.isdir(os.path.join(p, "etc")) and any([
                os.path.isdir(os.path.join(p, "lib64")),
                os.path.isdir(os.path.join(p, "lib")),
                os.path.isdir(os.path.join(p, "bin")),
                os.path.isfile(os.path.join(p, "build.prop")),
                os.path.isfile(os.path.join(p, "etc", "build.prop")),
            ]):
                found.append(os.path.realpath(p))
                dirs.remove(d)
for p in sorted(set(found)):
    print(p)
PY
        )
    fi
}

# Separate optional positional target path from extra flags forwarded to port_audiox.sh
EXPLICIT_TARGET=""
EXTRA_ARGS=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help)
            cat <<'EOF'
Usage: ./install.sh [target_folder] [port_audiox.sh options...]

Works on Linux and Android (Termux / shell with python3 + bash).

Behavior:
  1. If run in a workdir containing ./vendor, ./erofs/vendor, ./odm, or ./erofs/odm,
     automatically detects and patches the partition (preferring vendor when present).
  2. If no vendor/ or odm/ partition is found, prompts interactively for a folder to scan.
  3. Dynamically detects AIDL (IFactory) vs HIDL (32-bit/64-bit IEffectsFactory).
  4. Any extra flags (--aidl-mode direct|shim, --strip-dolby, --dry-run, etc.) are
     forwarded directly to scripts/port_audiox.sh.
EOF
            exit 0
            ;;
        -*)
            EXTRA_ARGS+=("$1")
            if [[ "$1" == "--aidl-mode" || "$1" == "--force-hal" || "$1" == "--config-dir" || "$1" == "--assets" || "$1" == "--source-odm" ]]; then
                shift
                EXTRA_ARGS+=("${1:-}")
            fi
            shift
            ;;
        *)
            if [[ -z "$EXPLICIT_TARGET" ]]; then
                EXPLICIT_TARGET="$1"
            else
                EXTRA_ARGS+=("$1")
            fi
            shift
            ;;
    esac
done

if [[ -n "$EXPLICIT_TARGET" ]]; then
    if [[ ! -d "$EXPLICIT_TARGET" ]]; then
        echo "[ERR] Directory does not exist: $EXPLICIT_TARGET" >&2
        exit 1
    fi
    scan_for_targets "$EXPLICIT_TARGET"
else
    scan_for_targets "$WORK_DIR"
fi

# Interactive prompt when no vendor/ or odm/ directory was auto-detected
while [[ ${#CANDIDATES[@]} -eq 0 ]]; do
    echo "[INFO] No unpacked /vendor or /odm directory found in: $WORK_DIR"
    if [[ ! -t 0 ]]; then
        echo "[ERR] Non-interactive shell and no vendor/odm found. Pass a target folder: ./install.sh <folder>" >&2
        exit 1
    fi
    read -r -p "Enter folder path to scan (or 'q' to quit): " scan_path
    if [[ "$scan_path" == "q" || "$scan_path" == "Q" ]]; then
        exit 0
    fi
    scan_path="${scan_path/#\~/$HOME}"
    if [[ ! -d "$scan_path" ]]; then
        echo "[WARN] Folder does not exist: $scan_path"
        continue
    fi
    scan_for_targets "$scan_path"
    if [[ ${#CANDIDATES[@]} -eq 0 ]]; then
        echo "[WARN] No vendor/ or odm/ partition root found under: $scan_path"
    fi
done

SELECTED=""
if [[ ${#CANDIDATES[@]} -eq 1 ]]; then
    SELECTED="${CANDIDATES[0]}"
else
    for c in "${CANDIDATES[@]}"; do
        if [[ "$(basename "$c")" == "vendor" ]]; then
            SELECTED="$c"
            break
        fi
    done
    if [[ -z "$SELECTED" ]]; then
        SELECTED="${CANDIDATES[0]}"
    fi
    echo "[INFO] Multiple partitions detected:"
    for idx in "${!CANDIDATES[@]}"; do
        echo "  $((idx + 1)). ${CANDIDATES[$idx]}"
    done
    if [[ -t 0 ]]; then
        read -r -p "Select target [1]: " sel_idx
        if [[ -n "$sel_idx" && "$sel_idx" =~ ^[0-9]+$ ]] && (( sel_idx >= 1 && sel_idx <= ${#CANDIDATES[@]} )); then
            SELECTED="${CANDIDATES[$((sel_idx - 1))]}"
        fi
    else
        echo "[INFO] Auto-selected: $SELECTED"
    fi
fi

echo "[EXEC] Target partition : $SELECTED"
echo "[EXEC] Prebuilt assets  : $ASSETS_DIR"
exec bash "$PORT_SCRIPT" --target "$SELECTED" --assets "$ASSETS_DIR" "${EXTRA_ARGS[@]}"
