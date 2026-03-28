#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
IMAGE_NAME="sir-launchalot-builder"

# Auto-detect container runtime
if command -v podman &>/dev/null; then
    RUNTIME=podman
elif command -v docker &>/dev/null; then
    RUNTIME=docker
else
    echo "ERROR: Neither podman nor docker found. Install one first."
    exit 1
fi

echo "=== Building AppImage in container (Ubuntu 22.04, glibc 2.35) ==="
echo "Using: $RUNTIME"

# Build the container image if needed
echo "Building container image..."
$RUNTIME build -t "$IMAGE_NAME" "$PROJECT_DIR"

# Run the build inside the container
# Mount the project as /src, output the AppImage to /src/build/
echo "Building AppImage inside container..."
$RUNTIME run --rm \
    -v "$PROJECT_DIR":/src:Z \
    --device /dev/fuse \
    --cap-add SYS_ADMIN \
    --security-opt apparmor:unconfined \
    "$IMAGE_NAME" \
    bash -c '
        set -euo pipefail
        cd /src
        # Clean previous build artifacts that may have host-linked objects
        rm -rf build/CMakeCache.txt build/CMakeFiles
        scripts/build-appimage.sh
    '

# Extract version for output message
VERSION=$(grep -oP 'project\(sir-launchalot VERSION \K[0-9.]+' "$PROJECT_DIR/CMakeLists.txt")
APPIMAGE="$PROJECT_DIR/build/Sir_Launchalot-${VERSION}-x86_64.AppImage"

if [ -f "$APPIMAGE" ]; then
    echo ""
    echo "=== AppImage ready: $APPIMAGE ==="
    echo "Built against glibc 2.35 (Ubuntu 22.04) for broad distro compatibility."
else
    echo "ERROR: AppImage was not created"
    exit 1
fi
