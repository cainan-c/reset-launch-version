# Reset Launch Version (kaiHEN homebrew)

This branch builds a small Switch 2 homebrew app using [ounce-devkit](https://github.com/ouncebrew/ounce-devkit). It lists application titles known to NS, their title IDs, and the launch version AVM currently reports. Select a title with the D-pad and press **A** to reset its launch version to `0` (`1.0.0`).

The reset is intended to let an older version of a game run after a newer version has been used, working around the Switch 2 anti-downgrade launch check. The title must be present on the Home Menu so NS has an application record for it. Resetting only permits the lowest game version available on the cartridge; it does not select arbitrary cartridge versions, install or remove updates, or alter save data.

## Build

This project uses the Ounce Switch 2 devkit's libnx-compatible rules. With devkitPro and the Ounce packages installed, run:

```sh
make
```

The output is `reset-launch-version.nro`. The project Makefile includes `$(DEVKITPRO)/libounce/switch2_rules` as specified by [ounce-devkit](https://github.com/ouncebrew/ounce-devkit#build-a-program).

## Use

1. Copy `reset-launch-version.nro` to the SD card's `/switch/` folder.
2. Launch it from your homebrew menu while kaiHEN is active.
3. Select a title with **Up/Down**.
4. Check its displayed name, 16-digit title ID, and current launch version.
5. Press **A** to reset that title to `1.0.0`. **B** or **Plus** exits.

The app lists NS application records. If title metadata is unavailable, the title ID is shown in place of its name. It only enables the reset when AVM returned a launch version for that title.

## References

- Goldleaf's [launch-version update path](https://github.com/XorTroll/Goldleaf/blob/1.2.1/Goldleaf/source/cnt/cnt_Content.cpp#L443-L455) and [NS IPC helper](https://github.com/XorTroll/libnx-ext/blob/main/libnx-ipcext/source/ns-ext.c#L500-L520) informed the AVM/NS call sequence; no source was copied verbatim.
- DBI's [documentation](https://github.com/rashevskyv/dbi) was consulted for its per-title “Reset required version” feature; no DBI source code was copied.
- AVM and NS APIs were cross-checked against [libnx](https://github.com/switchbrew/libnx) and [Switchbrew's AVM service notes](https://switchbrew.org/wiki/Shared_Database_services).

## License

MIT. See [`LICENSE`](LICENSE).
