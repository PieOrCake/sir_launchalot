# Tests

```bash
./tests/run-tests.sh            # build and run everything
./tests/run-tests.sh proton env # just the named ones
```

Builds into `build-tests/`, separate from the normal `build/`, so running tests
never disturbs the app build or repackages the AppImage. Tests are off by
default in the main build; `-DBUILD_TESTS=ON` turns them on.

Each harness is a plain executable that prints its own results and exits
non-zero on failure, so it can be run directly as well as through `ctest`.

## What each one covers

| Test | Covers |
| --- | --- |
| `proton` | Proton version parsing and selection, including the `-x86_64` suffixed folders that issue #8 reported being ignored, numeric ordering (11-10 beats 11-2), and skipping a Proton whose runtime is missing. |
| `account` | Account JSON round-tripping, and the `lastSeen` rule that takes the later of the API timestamp and the locally recorded end of a session. |
| `addon` | Addon settings-folder detection — in particular that an addon cannot be given a folder already owned by a different installed addon, which is what sent SerenadeSpike's music to the wrong place. |
| `env` | Per-account environment variables: parsing `NAME=value` as people paste it, refusing launcher-managed names like `WINEPREFIX`, and the `export` lines written into the launch script. |
| `ui-context-menu` | Drives the real `MainWindow`: right-clicks an account, rebuilds the list behind the open menu, then chooses Edit and checks the dialog is for the account that was right-clicked. Guards against the stale `QListWidgetItem` pointers that caused the context-menu crash. |
| `prefix-sync` | The rsync exclude rules, via real files on disk: an alt's chosen addons and its own settings survive a relaunch sync, unticked addons never appear, and a game patch still propagates. |

## Notes

- `ui-context-menu` runs against a throwaway copy of `fixtures/accounts.json`
  with `XDG_CONFIG_HOME` redirected, so it cannot see or touch a real config,
  and starts clean every run. It forces Qt's `offscreen` platform — on a live
  desktop the window manager interferes with the nested menu event loop and the
  test cannot read the menu's actions.
- The account id `12e5450e` in the fixture is what the harness looks for. If you
  change it, change it in `ui-context-menu/main.cpp` too.
- These do not run automatically on build or on commit.
