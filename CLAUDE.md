# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Collaboration Notes

When the user says "discuss" or "suggest", respond with conversation only — do not write or modify code.

Never add "Co-Authored-By" trailers to commits or release notes.

## What This Is

Sir Launchalot is a C++17/Qt6 GUI application for running multiple Guild Wars 2 accounts simultaneously (multiboxing) on Linux. It auto-detects GW2 installations from Lutris, Heroic, Faugus, and Steam; clones Wine prefixes per alt account (sharing the large `Gw2.dat` via symlink); and launches accounts via `umu-launcher` with Proton.

## Build Commands

```bash
# Build (always use this — builds the binary and creates the AppImage)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && make -C build -j$(nproc)

# Build distributable AppImage (container — recommended for compatibility)
./scripts/build-appimage-docker.sh

# Build AppImage on host
./scripts/build-appimage.sh
```

**Always build via the AppImage target** (`make -C build -j$(nproc)`) so the AppImage is produced alongside the binary. Never build just the binary alone.

There are no automated tests and no linting/formatting configuration in this repo.

## Architecture

**Pattern:** Manager-based core (`src/core/`) separated from Qt UI layer (`src/ui/`). Qt signals/slots are used for asynchronous operations throughout.

### Core Managers (`src/core/`)

- **AccountManager** — Persists account list to JSON config. Stores credentials, graphics settings, addon toggles, per-account env vars, and GW2 API keys. Differentiates main account from alts.
- **ProcessManager** — Drives the full account lifecycle: prefix cloning via rsync, umu-run process execution, instance state machine (Stopped → Starting → Running → Stopping), setup mode (captures alt credentials), update mode (`-image` flag for `Local.dat` refresh), and patch detection by watching `Gw2.dat` mtime.
- **InstallDetector** — Scans Lutris, Heroic, Faugus, and Steam for GW2 installations; extracts Wine binary, prefix path, executable location, and PROTONPATH for umu-launcher.
- **WineManager** — Discovers available Wine runners (system, Lutris, Proton) and selects the best match for a given prefix.
- **OverlayManager** — Manages the data directory layout for the overlay/addons system.
- **Gw2ApiClient** — Fetches account info and Wizard's Vault progress from the GW2 REST API; caches results with in-flight request deduplication.

### UI Layer (`src/ui/`)

- **MainWindow** — Primary window: account list with drag-drop reordering, per-account launch/stop controls, external app launcher, log viewer panel, and API refresh timer.
- **SetupWizard** — First-run flow: auto-detection, manual configuration, account naming.
- **AltSetupWizard** — Add-alt workflow.
- **AccountDialog** — Edit an existing account's settings.
- **SettingsDialog** — Application-wide settings.

### Key Runtime Details

- Config is stored in `~/.config/sir-launchalot/` (or `sir-launchalot-dev/` when launched with `--dev`).
- Alts use rsync-cloned Wine prefixes; `Gw2.dat` is shared via symlink to avoid duplicating the ~30 GB file.
- `main.cpp` checks for `--dev` flag and sets a separate config path before constructing `QApplication`.

## Dependencies

Runtime: `umu-launcher`, `rsync`, Qt6 (Core, Gui, Widgets, Network).  
Build: CMake 3.16+, C++17 compiler, Qt6 dev packages.  
Packaging: `linuxdeploy` (downloaded by build script), optionally podman/docker for container builds.
