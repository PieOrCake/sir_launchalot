#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_DIR/build"
APPDIR="$BUILD_DIR/AppDir"
TOOLS_DIR="$BUILD_DIR/tools"

SKIP_BUILD=false
for arg in "$@"; do
    case "$arg" in
        --skip-build) SKIP_BUILD=true ;;
    esac
done

echo "=== Sir Launchalot AppImage Builder ==="

# Extract version from CMakeLists.txt
VERSION=$(grep -oP 'project\(sir-launchalot VERSION \K[0-9.]+' "$PROJECT_DIR/CMakeLists.txt")
if [ -z "$VERSION" ]; then
    echo "ERROR: Could not extract version from CMakeLists.txt"
    exit 1
fi
echo "Version: $VERSION"

# Clean previous AppDir and AppImage
rm -rf "$APPDIR"
rm -f "$BUILD_DIR"/Sir_Launchalot*.AppImage
mkdir -p "$BUILD_DIR" "$TOOLS_DIR"

# Download linuxdeploy if not cached
LINUXDEPLOY="$TOOLS_DIR/linuxdeploy-x86_64.AppImage"

if [ ! -f "$LINUXDEPLOY" ]; then
    echo "Downloading linuxdeploy..."
    curl -fSL -o "$LINUXDEPLOY" \
        "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage"
    chmod +x "$LINUXDEPLOY"
fi

if [ "$SKIP_BUILD" = false ]; then
    # Build the project
    echo "Building project..."
    cmake -B "$BUILD_DIR" -S "$PROJECT_DIR" -DCMAKE_BUILD_TYPE=Release
    cmake --build "$BUILD_DIR" --parallel "$(nproc)"
fi

# Install into AppDir
echo "Installing into AppDir..."
DESTDIR="$APPDIR" cmake --install "$BUILD_DIR"

# linuxdeploy's bundled strip is too old to handle .relr.dyn sections from
# modern toolchains (GCC 15+, binutils 2.43+). Extract linuxdeploy and replace
# its strip with the system one.
echo "Preparing linuxdeploy..."
LINUXDEPLOY_EXTRACTED="$TOOLS_DIR/linuxdeploy-extracted"
if [ ! -d "$LINUXDEPLOY_EXTRACTED" ]; then
    pushd "$TOOLS_DIR" > /dev/null
    "$LINUXDEPLOY" --appimage-extract > /dev/null 2>&1
    mv squashfs-root "$LINUXDEPLOY_EXTRACTED"
    popd > /dev/null
fi
cp "$(which strip)" "$LINUXDEPLOY_EXTRACTED/usr/bin/strip"

# Manually deploy Qt plugins (skip linuxdeploy-plugin-qt to avoid it pulling in
# every KDE image format plugin with heavy deps like libheif, libavif, OpenEXR).
echo "Deploying Qt plugins..."
QT_PLUGIN_PATH=$(qmake6 -query QT_INSTALL_PLUGINS 2>/dev/null || echo "/usr/lib/qt6/plugins")
QT_LIB_PATH=$(qmake6 -query QT_INSTALL_LIBS 2>/dev/null || echo "/usr/lib")

APPDIR_PLUGINS="$APPDIR/usr/plugins"
mkdir -p "$APPDIR_PLUGINS"

# Only deploy the plugin categories we actually need
NEEDED_PLUGIN_DIRS="platforms iconengines imageformats wayland-shell-integration wayland-decoration-client wayland-graphics-integration-client xcbglintegrations"

for plugdir in $NEEDED_PLUGIN_DIRS; do
    src="$QT_PLUGIN_PATH/$plugdir"
    [ ! -d "$src" ] && continue
    mkdir -p "$APPDIR_PLUGINS/$plugdir"
    for plugin in "$src"/*.so; do
        [ ! -f "$plugin" ] && continue
        name=$(basename "$plugin")
        # Skip KDE image format plugins
        [[ "$name" =~ ^kimg_ ]] && continue
        cp "$plugin" "$APPDIR_PLUGINS/$plugdir/"
    done
done

# Create qt.conf so Qt finds plugins in the AppDir
mkdir -p "$APPDIR/usr/bin"
cat > "$APPDIR/usr/bin/qt.conf" << 'EOF'
[Paths]
Prefix = ../
Plugins = plugins
Libraries = lib
EOF

export DISABLE_COPYRIGHT_FILES_DEPLOYMENT=1
export OUTPUT="$BUILD_DIR/Sir_Launchalot-${VERSION}-x86_64.AppImage"

echo "Creating AppImage..."

# Build linuxdeploy --library args for all manually-deployed plugin .so files
DEPLOY_LIBS=()
while IFS= read -r -d '' sofile; do
    DEPLOY_LIBS+=(--deploy-deps-only "$sofile")
done < <(find "$APPDIR_PLUGINS" -name '*.so' -print0)

"$LINUXDEPLOY_EXTRACTED/AppRun" \
    --appdir "$APPDIR" \
    "${DEPLOY_LIBS[@]}" \
    --desktop-file "$APPDIR/usr/share/applications/sir-launchalot.desktop" \
    --icon-file "$APPDIR/usr/share/icons/hicolor/scalable/apps/sir-launchalot.svg" \
    --output appimage

echo ""
echo "=== AppImage created: $OUTPUT ==="
echo "Run with: chmod +x '$OUTPUT' && '$OUTPUT'"

# Create unversioned symlink for convenience
ln -sf "$(basename "$OUTPUT")" "$BUILD_DIR/Sir_Launchalot-x86_64.AppImage"
