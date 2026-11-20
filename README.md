# Reset Launch Version

Reset Launch Version resets the recorded launch version of Switch 2 applications.
It is intended to let an older game version run after a newer version has been
used, working around the Switch 2's anti-downgrade launch check. Version code
`0` is the base application version, `1.0.0`.

The title must be present on the Home Menu when you run the reset, so NS has an
application record to target. Resetting its launch version to `0` only permits
running the lowest game version available on the cartridge; it does not let you
choose an arbitrary version or run a version that is not on the cartridge.

The tool updates the launch-version records through AVM and NS. It does not
uninstall updates, change save data, or alter the installed game content.

## Requirements

- A working, held ns2jb RPC session on the same console boot. Keep SmileBASIC
  open while using it.
- Node.js 22 or newer on the PC.
- The ns2jb checkout used to start the held session. This CJS file uses its
  existing session and RPC helpers; it is not a standalone console client.

## Setup

In a terminal, point the script at your ns2jb checkout and active run directory. Replace `RUNID` with the folder name printed by ns2jb:

```sh
export NS2JB_ROOT="/path/to/ns2jb"
export NS2_RUN_DIR="$NS2JB_ROOT/runs/switch2-RUNID"
```

If this file is placed at
`<ns2jb checkout>/engine/switch2/app/reset-launch-version.cjs`, the
`NS2JB_ROOT` setting can be omitted.

## Usage

Preview all application records known to NS (no version records are changed):

```sh
node ./reset-launch-version.cjs --all
```

Reset all those titles to version code `0` (`1.0.0`):

```sh
node ./reset-launch-version.cjs --all --apply
```

To process one title, provide its 16-digit hexadecimal application ID:

```sh
node ./reset-launch-version.cjs 0100000000001000 --apply
```

The default target version is `0`. `--version <number>` can select another
uint32 version code. `--apply` is required to write changes; omitting it leaves
the operation in preview mode. The script prints the application IDs and
results as JSON. For applied runs, it also writes a progress record under
`<run directory>/switch2/launch-version-reset-*.json`.

`--all` means every application record returned by NS on that console, not
every title ever released. Review the preview before applying if you only
intend to change a few games.

## References

- Goldleaf's [content update code](https://github.com/XorTroll/Goldleaf/blob/1.2.1/Goldleaf/source/cnt/cnt_Content.cpp#L443-L455)
  and [NS IPC helper](https://github.com/XorTroll/libnx-ext/blob/main/libnx-ipcext/source/ns-ext.c#L500-L520)
  informed the AVM/NS launch-version call sequence. No Goldleaf source was
  copied verbatim.
- DBI's [documentation](https://github.com/rashevskyv/dbi) was consulted for
  its per-title “Reset required version” feature and terminology. No DBI source
  code was copied.

## License

This project is licensed under the MIT License. See [LICENSE](LICENSE).
