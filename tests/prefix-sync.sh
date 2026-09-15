#!/usr/bin/env bash
# Throwaway harness for the selective-addon sync rules.
#
# Checks the two things the launch path relies on but cannot easily be seen from
# the app: that the anchored excludes keep an alt's curated addons folder out of
# the prefix clone (game-in-prefix layout), and that the addon-free game dir sync
# leaves the alt's symlinks and settings alone (game-outside-prefix layout).
set -u

PASS=0; FAIL=0
ok()   { PASS=$((PASS+1)); echo "  ok    $1"; }
bad()  { FAIL=$((FAIL+1)); echo "  FAIL  $1"; }
check(){ if eval "$1"; then ok "$2"; else bad "$2"; fi; }

ROOT=$(mktemp -d)
trap 'rm -rf "$ROOT"' EXIT

REL="drive_c/Program Files/Guild Wars 2"

make_main_prefix() {
  local P="$1"
  mkdir -p "$P/$REL/addons/Nexus" "$P/$REL/addons/PieUI" "$P/$REL/addons/FlipOut"
  mkdir -p "$P/drive_c/windows/system32" "$P/drive_c/users/steamuser"
  echo "nexus"      > "$P/$REL/d3d11.dll"
  echo "wine-d3d11" > "$P/drive_c/windows/system32/d3d11.dll"
  echo "game"       > "$P/$REL/Gw2-64.exe"
  echo "archive"    > "$P/$REL/Gw2.dat"
  echo "pieui"      > "$P/$REL/addons/PieUI.dll"
  echo "flipout"    > "$P/$REL/addons/FlipOut.dll"
  echo "taimi"      > "$P/$REL/addons/TaimiHUD.dll"
  echo "main-settings" > "$P/$REL/addons/PieUI/settings.json"
  echo "main-layout"   > "$P/$REL/addons/Nexus/imgui.ini"
}

# --- Layout 1: game inside the prefix (Lutris) ------------------------------
echo
echo "[game inside prefix] clone must not touch the alt's addons folder"

MAIN="$ROOT/main"; CLONE="$ROOT/clone"
make_main_prefix "$MAIN"
mkdir -p "$CLONE"

clone_sync() {
  rsync -a --delete \
    --exclude "Gw2.dat" --exclude "Gw2.tmp" --exclude "Gw2-64.tmp" \
    --exclude "/$REL/addons/" --exclude "/$REL/d3d11.dll" \
    "$MAIN/" "$CLONE/"
}

clone_sync
check '[ ! -e "$CLONE/$REL/addons" ]'  "main's addons folder is not copied into the clone"
check '[ ! -e "$CLONE/$REL/d3d11.dll" ]' "Nexus loader is not copied into the clone"
check '[ -f "$CLONE/drive_c/windows/system32/d3d11.dll" ]' \
      "Wine's own d3d11.dll is untouched by the anchored exclude"
check '[ -f "$CLONE/$REL/Gw2-64.exe" ]' "game exe is copied"
check '[ ! -e "$CLONE/$REL/Gw2.dat" ]'  "Gw2.dat is still excluded"

# What the app then builds inside the clone: links for the chosen addons only,
# plus a seeded settings folder.
mkdir -p "$CLONE/$REL/addons"
ln -s "$MAIN/$REL/addons/PieUI.dll" "$CLONE/$REL/addons/PieUI.dll"
ln -s "$MAIN/$REL/d3d11.dll"        "$CLONE/$REL/d3d11.dll"
cp -a "$MAIN/$REL/addons/PieUI"     "$CLONE/$REL/addons/PieUI"
touch "$CLONE/$REL/addons/.sir-launchalot"
echo "alt-tweaked" > "$CLONE/$REL/addons/PieUI/settings.json"

clone_sync
check '[ -L "$CLONE/$REL/addons/PieUI.dll" ]' "alt's addon link survives a relaunch sync"
check '[ -L "$CLONE/$REL/d3d11.dll" ]'        "Nexus loader link survives a relaunch sync"
check '[ ! -e "$CLONE/$REL/addons/FlipOut.dll" ]' "unticked addon never appears"
check '[ "$(cat "$CLONE/$REL/addons/PieUI/settings.json")" = "alt-tweaked" ]' \
      "alt's own settings are not overwritten by main's"
check '[ -f "$CLONE/$REL/addons/.sir-launchalot" ]' "seed marker survives"

echo "  (a new addon installed on main must not leak in)"
echo "newthing" > "$MAIN/$REL/addons/NewAddon.dll"
clone_sync
check '[ ! -e "$CLONE/$REL/addons/NewAddon.dll" ]' "new addon on main does not reach the alt"

# --- Layout 2: game outside the prefix (Faugus/Heroic) ----------------------
echo
echo "[game outside prefix] addon-free game dir must leave the alt's addons alone"

GAME="$ROOT/game"; ALT="$ROOT/altgame"
mkdir -p "$GAME/addons/Nexus" "$GAME/addons/PieUI" "$GAME/bin64"
echo "nexus"   > "$GAME/d3d11.dll"
echo "game"    > "$GAME/Gw2-64.exe"
echo "archive" > "$GAME/Gw2.dat"
echo "cef"     > "$GAME/bin64/cef.dll"
echo "pieui"   > "$GAME/addons/PieUI.dll"
echo "main-settings" > "$GAME/addons/PieUI/settings.json"
mkdir -p "$ALT"

strip_sync() {
  rsync -a --delete \
    --exclude "/Gw2.dat" --exclude "/Gw2.tmp" --exclude "/Gw2-64.tmp" \
    --exclude "/debug.log" --exclude "/Crash.dmp" \
    --exclude "/d3d11.dll" --exclude "/d3d9.dll" --exclude "/dxgi.dll" \
    --exclude "/ReShade.ini" --exclude "/GW2-UOAOM.ini" --exclude "/arcdps.ini" \
    --exclude "/addons/" --exclude "/reshade-shaders/" \
    "$GAME/" "$ALT/"
}

strip_sync
check '[ -f "$ALT/Gw2-64.exe" ]'     "game exe copied to the alt"
check '[ -f "$ALT/bin64/cef.dll" ]'  "ArenaNet's CEF browser survives"
check '[ ! -e "$ALT/addons" ]'       "addons folder not copied"
check '[ ! -e "$ALT/d3d11.dll" ]'    "Nexus loader not copied"

mkdir -p "$ALT/addons"
ln -s "$GAME/addons/PieUI.dll" "$ALT/addons/PieUI.dll"
ln -s "$GAME/d3d11.dll"        "$ALT/d3d11.dll"
ln -s "$GAME/Gw2.dat"          "$ALT/Gw2.dat"
cp -a "$GAME/addons/PieUI"     "$ALT/addons/PieUI"
touch "$ALT/addons/.sir-launchalot"
echo "alt-tweaked" > "$ALT/addons/PieUI/settings.json"
echo "alt log"     > "$ALT/debug.log"

strip_sync
check '[ -L "$ALT/addons/PieUI.dll" ]' "addon link survives the next launch"
check '[ -L "$ALT/d3d11.dll" ]'        "Nexus loader link survives the next launch"
check '[ -L "$ALT/Gw2.dat" ]'          "Gw2.dat symlink survives the next launch"
check '[ "$(cat "$ALT/addons/PieUI/settings.json")" = "alt-tweaked" ]' \
      "alt keeps its own settings across launches"
check '[ "$(cat "$ALT/debug.log")" = "alt log" ]' "alt keeps its own debug.log"

echo "  (a game patch must still propagate)"
echo "patched" > "$GAME/Gw2-64.exe"
strip_sync
check '[ "$(cat "$ALT/Gw2-64.exe")" = "patched" ]' "patched exe reaches the alt"

# --- Link behaviour ---------------------------------------------------------
echo
echo "[links] reading through a link sees main's current file"
echo "v2" > "$GAME/addons/PieUI.dll"
check '[ "$(cat "$ALT/addons/PieUI.dll")" = "v2" ]' "updated addon is picked up with no re-copy"

rm "$GAME/addons/PieUI.dll"
check '[ -L "$ALT/addons/PieUI.dll" ] && [ ! -e "$ALT/addons/PieUI.dll" ]' \
      "uninstalling on main leaves a dangling link (pruned by the app)"

echo
echo "$PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
