# Sidecar Programs — Design Spec
_2026-05-14_

## Overview

Allow the main GW2 account to launch one or more auxiliary Windows executables alongside GW2, running inside the same Wine/Proton prefix. Primary use case: `winediscordipcbridge.exe` for Discord Rich Presence, but the feature is generic.

Sidecars apply to the **main account only**. Alt accounts are unaffected.

---

## Data Model

New struct added to `AccountManager`:

```
SidecarProgram {
    id       : QString   // UUID, generated on creation
    name     : QString   // display label (e.g. "Discord IPC Bridge")
    exePath  : QString   // Windows-style path (e.g. C:\tools\winediscordipcbridge.exe)
    args     : QStringList  // optional command-line arguments
}
```

The main `Account` struct gains a `QList<SidecarProgram> sidecars` field. Serialised as a JSON array. Alts never read or write this field.

---

## UI

A **"Sidecar Programs"** section is added to `AccountDialog`, visible only when editing the main account (`acct.isMain == true`).

- A `QTableWidget` lists current sidecars (columns: Name, Exe Path).
- **Add** button opens a small `QDialog` with fields: Name, Exe Path, Arguments.
- **Remove** button deletes the selected row.
- Double-clicking a row re-opens the edit dialog.

No changes to any other dialog or window.

---

## Launch Flow (`ProcessManager`)

In `launchAccount`, after confirming `acct.isMain` and before writing the GW2 launch script:

1. Iterate `acct.sidecars`.
2. For each entry, call a new helper `launchSidecar(accountId, sidecar, basePrefix)` which:
   - Constructs a Windows→Wine path translation for `exePath`.
   - Spawns `umu-run` with the same `WINEPREFIX`, `PROTONPATH`, and env vars as the main GW2 launch.
   - Stores the resulting `QProcess*` in `m_sidecars[accountId]`.
3. GW2 is then launched as normal.

Sidecar `QProcess` signals (`finished`, `errorOccurred`) are connected to a slot that logs the event but takes no lifecycle action — a sidecar dying does not affect GW2.

---

## Teardown

When the GW2 `QProcess` emits `finished` for the main account, the existing cleanup code is extended to:

1. Iterate `m_sidecars[accountId]`.
2. Call `terminate()` on each; follow with `kill()` after a short timeout if still running.
3. Clear the list.

---

## Data Storage

`AccountManager` gains `SidecarProgram` serialisation alongside the existing account JSON read/write. No migration needed — missing `sidecars` key deserialises as an empty list.

---

## Out of Scope

- Sidecar support for alt accounts.
- Auto-downloading or bundling any sidecar exe.
- Showing per-sidecar status in the main window.
