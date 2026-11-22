#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK_DIR="$(pwd)"

# Locate port_audiox.sh and prebuilt assets
PORT_SCRIPT=""
ASSETS_DIR=""
for cand in "$SCRIPT_DIR/scripts/port_audiox.sh" "$SCRIPT_DIR/port_audiox.sh" "/mnt/project/RomTools/tool/tmp/oplus/mybin/port_audiox.sh"; do
    if [[ -x "$cand" ]]; then
        PORT_SCRIPT="$cand"
        break
    fi
done

for cand in "$SCRIPT_DIR/prebuilt" "$SCRIPT_DIR" "/mnt/project/RomTools/tool/tmp/oplus/mybin"; do
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

scan_for_targets() {
    local base="$1"
    local -n out_arr=$2
    out_arr=()

    [[ -d "$base" ]] || return 0
    local abs_base
    abs_base="$(cd "$base" && pwd)"

    # If the given directory itself is already a vendor or odm root
    local bname
    bname="$(basename "$abs_base")"
    if [[ "$bname" == "vendor" || "$bname" == "odm" ]] && is_partition_root "$abs_base"; then
        out_arr+=("$abs_base")
        return 0
    fi

    # Check common direct & RomTools layouts first (vendor preferred over odm)
    local cand
    for cand in \
        "$abs_base/vendor" \
        "$abs_base/erofs/vendor" \
        "$abs_base/odm" \
        "$abs_base/erofs/odm"; do
        if is_partition_root "$cand"; then
            out_arr+=("$(cd "$cand" && pwd)")
        fi
    done

    # Fallback shallow scan (depth <= 3) if nothing found in direct layouts
    if [[ ${#out_arr[@]} -eq 0 ]]; then
        local found_dir
        while IFS= read -r -d '' found_dir; do
            local clean="${found_dir%/}"
            if is_partition_root "$clean"; then
                out_arr+=("$(cd "$clean" && pwd)")
            fi
        done < <(fd -t d -d 3 '^(vendor|odm)$' "$abs_base" -0 2>/dev/null | sort -z)
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

Behavior:
  1. If run in a workdir containing ./vendor, ./erofs/vendor, ./odm, or ./erofs/odm,
     automatically detects and patches the partition (preferring vendor when present).
  2. If no vendor/ or odm/ partition is found, prompts interactively for a folder to scan.
  3. Any extra flags (--aidl-mode direct|shim, --strip-dolby, --dry-run, etc.) are
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

CANDIDATES=()
if [[ -n "$EXPLICIT_TARGET" ]]; then
    if [[ ! -d "$EXPLICIT_TARGET" ]]; then
        echo "[ERR] Directory does not exist: $EXPLICIT_TARGET" >&2
        exit 1
    fi
    scan_for_targets "$EXPLICIT_TARGET" CANDIDATES
else
    # Scan current working directory where install.sh was invoked
    scan_for_targets "$WORK_DIR" CANDIDATES
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
    # Expand leading ~
    scan_path="${scan_path/#\~/$HOME}"
    if [[ ! -d "$scan_path" ]]; then
        echo "[WARN] Folder does not exist: $scan_path"
        continue
    fi
    scan_for_targets "$scan_path" CANDIDATES
    if [[ ${#CANDIDATES[@]} -eq 0 ]]; then
        echo "[WARN] No vendor/ or odm/ partition root found under: $scan_path"
    fi
done

# Pick target: auto-pick if single match or if vendor is present in a standard vendor+odm pair
SELECTED=""
if [[ ${#CANDIDATES[@]} -eq 1 ]]; then
    SELECTED="${CANDIDATES[0]}"
else
    # Prefer vendor over odm automatically if both are in the same parent dir
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
exec "$PORT_SCRIPT" --target "$SELECTED" --assets "$ASSETS_DIR" "${EXTRA_ARGS[@]}"
