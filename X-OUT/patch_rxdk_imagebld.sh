#!/bin/bash
# Helper script to apply the imagebld absolute path wrapper fix for RXDK on Linux.
# Re-run this script if RXDK updates or re-installs its host tools. Set RXDK_ROOT if the
# extension keeps its data somewhere other than the Flatpak VS Code default below.

RXDK_ROOT="${RXDK_ROOT:-${HOME}/.var/app/com.visualstudio.code/data/rxdk}"
TOOLS_DIR="${RXDK_ROOT}/tools"
IMAGEBLD="${TOOLS_DIR}/imagebld"
IMAGEBLD_REAL="${TOOLS_DIR}/imagebld.real"

if [ ! -f "$IMAGEBLD" ]; then
    echo "Error: imagebld not found at $IMAGEBLD"
    exit 1
fi

# Backup original binary if not already backed up
if [ ! -f "$IMAGEBLD_REAL" ]; then
    echo "Backing up original imagebld to imagebld.real..."
    mv "$IMAGEBLD" "$IMAGEBLD_REAL"
elif file "$IMAGEBLD" | grep -q "ELF"; then
    echo "ELF binary found at imagebld; refreshing imagebld.real..."
    mv "$IMAGEBLD" "$IMAGEBLD_REAL"
fi

echo "Writing imagebld Linux wrapper script..."
cat << 'EOF' > "$IMAGEBLD"
#!/bin/bash
REAL="$(dirname "$0")/imagebld.real"

args=()
target_dir=""

for arg in "$@"; do
    if [[ "$arg" == /* && ! "$arg" =~ ^/(DXT|IN:|OUT:|LIMITMEM|DONT|UD|TEST|TITLE|FORMAT|INSERT|NOPRELOAD|STACK|\?) ]]; then
        if [ -z "$target_dir" ]; then
            target_dir="$(dirname "$arg")"
        fi
        args+=("$(basename "$arg")")
    else
        args+=("$arg")
    fi
done

if [ -n "$target_dir" ] && [ -d "$target_dir" ]; then
    (cd "$target_dir" && "$REAL" "${args[@]}")
    exit $?
else
    exec "$REAL" "$@"
fi
EOF

chmod +x "$IMAGEBLD"
echo "Fix applied successfully! imagebld now safely handles Linux absolute paths."
