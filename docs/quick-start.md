# Add ZMK Next to a ZMK config

ZMK Next is a ZMK fork with an experimental Runtime Config service. You keep
your existing config repository, boards, shields, keymap, and normal build
workflow. The firmware exposes supported keymap changes to the local ZMK Next
Configurator over ZMK Studio's USB serial connection.

Adding ZMK Next takes two edits: one in `config/west.yml` and one in
`build.yaml`. The guide pins the `v0.2-branch` release line: each rebuild picks up the
latest v0.2.x fixes, and nothing that changes the configurator protocol arrives
until you change the pin. Use a configurator release with the same major and
minor version (any v0.2.x configurator works with any v0.2.x firmware).

## Tested keyboards and limits

ZMK Next has been checked on two keyboards so far:

| Keyboard | Board | Checked how |
|---|---|---|
| Totem (split) | Seeed XIAO BLE | Built from this guide and used daily |
| Corne (split) | nice!nano | Built with the snippet on every weekly release; not run on hardware |

Other keyboards should work if they meet these requirements, but they are untested:

- **Current ZMK.** ZMK Next follows upstream ZMK `main` (Zephyr 4.1). A config
  written for ZMK v0.2 or older must use the newer board names
  (`nice_nano//zmk`, not `nice_nano_v2`), and any third-party modules
  (displays, pointing devices) must support Zephyr 4.1.
- **USB on the central half.** The configurator talks to the keyboard over
  USB, so the half with the snippet needs USB and has to be plugged in.
  Boards without USB can't use it.
- **ZMK Studio's requirements:** a physical layout with `keys` (see below)
  and room for Studio and Runtime Config, which adds flash and RAM use. Small
  chips may not fit.

If you try another keyboard, please report how it went in an issue.

## 1. Point `west.yml` at ZMK Next

In your config repo's `config/west.yml`, replace the existing `zmk` project
entry while preserving your other modules and the `self` section:

```yaml
manifest:
  remotes:
    - name: zmk-next
      url-base: https://github.com/rleyvasal
  projects:
    - name: zmk
      remote: zmk-next
      repo-path: zmk-next
      revision: v0.2-branch
      import: app/west.yml
  self:
    path: config
```

The imported ZMK Next manifest brings in the matching protocol messages module,
so your config does not need to list `zmk-next-messages` separately.

## 2. Add the `zmk-next` snippet in `build.yaml`

Add `snippet: zmk-next` to the build that is physically connected by USB. The
snippet turns on ZMK Studio and Runtime Config and attaches the Studio
connection to the USB serial port, so no `cmake-args` are needed:

```yaml
include:
  - board: your_board
    shield: your_shield
    snippet: zmk-next
```

For a split keyboard, add it only to the central half. The peripheral half is
built from the same revision without the snippet:

```yaml
include:
  - board: your_board
    shield: your_keyboard_left
    snippet: zmk-next
  - board: your_board
    shield: your_keyboard_right
```

Replace the board and shield names with the values already used by your config.
If the right half is your central half, put the snippet there instead.

### Studio prerequisites

These are standard ZMK Studio requirements and normally need to be handled only
once:

- The keyboard needs a physical layout with a `keys` property. Most keyboards
  already prepared for ZMK Studio have one.
- Studio is locked by default. Keep your existing `&studio_unlock` key/combo,
  or temporarily add `-DCONFIG_ZMK_STUDIO_LOCKING=n` while evaluating ZMK Next.
  Don't wrap the unlock key in `#ifdef CONFIG_ZMK_STUDIO`: the snippet turns
  Studio on in Kconfig, which the keymap can't see, so the key would be left
  out and Studio would stay locked.
- The `zmk-next` snippet puts Studio on the board's USB serial port, so the
  keyboard shows up as one serial port, also on boards such as the Seeed XIAO
  BLE that already have one. It turns off the UART console on that port. A board with a custom or
  conflicting USB CDC setup can instead keep its own Studio RPC snippet and add
  `cmake-args: -DCONFIG_ZMK_STUDIO=y -DCONFIG_ZMK_RUNTIME_CONFIG=y`.

To create layers later from the configurator, reserve the desired number of
empty layer slots in the keymap. For example:

```dts
reserved_1 { status = "reserved"; };
reserved_2 { status = "reserved"; };
```

Without reserved slots, existing layers can still be edited, but the firmware's
fixed layer capacity cannot grow at runtime.

## 3. Build and flash

Commit the config change and let the config repo's existing GitHub Actions build
run, or build locally with your usual ZMK workflow. Flash every half that was
rebuilt. Connect the central half to the computer over USB.

## 4. Run the configurator locally

Download a configurator archive with the same major and minor version from the
[configurator Releases page](https://github.com/rleyvasal/zmk-next-configurator/releases),
extract it, then launch:

- macOS: `start.command`
- Linux: `start.sh`
- Windows: `start.bat`

Python 3 is the only local requirement. The launcher serves the app on
`127.0.0.1` and opens it in your browser; it does not require a VPS, cloud
backend, or installation. Chromium-based browsers currently provide the Web
Serial connection used by **Connect**. If Studio is locked, the configurator
asks you to press your unlock key and continues once you do (configurator
v0.2.2 or later).

After connecting, make a small key change and confirm that it types immediately.
Use **Restore stock configuration** to remove the runtime overlay and return to
the compiled keymap.

## 5. Optional: keyboard log and battery levels

The configurator can also show the keyboard's log and both halves' battery
levels over the same USB connection. These come from the
[zmk-studio-log](https://github.com/rleyvasal/zmk-studio-log) module and need
configurator v0.2.2 or later.

Add the module to `config/west.yml`, next to the `zmk` entry from step 1:

```yaml
- name: zmk-studio-log
  remote: zmk-next
  revision: v0.1-branch
```

Then turn it on in `config/<keyboard>.conf`:

```ini
CONFIG_ZMK_STUDIO_LOG=y

# Battery levels for the header chips
CONFIG_ZMK_BATTERY_HOLD_ON_EXTERNAL_POWER=y
# Split keyboards: also show the peripheral half's level
CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING=y
```

Rebuild and flash, connect, then click **Enable log** in the keyboard log
panel. The battery chips fill in within about 10 seconds.

- `CONFIG_ZMK_BATTERY_HOLD_ON_EXTERNAL_POWER` keeps the last level measured
  on battery while USB is plugged in. On USB the cell sits at charge voltage
  and would otherwise always read about 100%.
- The module only streams over USB, so a keyboard on Bluetooth sends nothing.
- On a split, the peripheral half's build warns that `CONFIG_ZMK_STUDIO_LOG`
  and the fetching option were not applied. That is expected: only the central
  half uses them.
- For tracking down unexpected restarts, the module also has restart
  diagnostics and a flash-backed black box. See its README.

## Updating later

ZMK Next merges upstream ZMK every week and tags a patch release (for example
v0.2.4) when the merge passes its tests, then moves the `v0.2-branch` branch to
it. A config pinned to `v0.2-branch` gets it on its next build; rebuild and
flash to update. zmk-studio-log's `v0.1-branch` works the same way. Patch
releases never change the configurator protocol.

Pin the branch, not the `v0.2` tag. The tag moves too, but west does not fetch
a tag it already has, and GitHub Actions builds reuse a cached west workspace
until `west.yml` changes. A config pinned to `v0.2` stays on the release it
first built with; change it to `v0.2-branch` (and `v0.1` to `v0.1-branch`).

A new minor version (v0.3) can change the protocol and needs a matching
configurator release, so moving to it is a deliberate edit: change
`v0.2-branch` to `v0.3-branch` and download the v0.3 configurator.

For builds that never change, pin an exact release (`v0.2.3`) instead and update
it by hand. Rolling back is pinning the previous release and rebuilding.
`main` also works and has the newest changes first, but it can break between
releases.
