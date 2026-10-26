# Add ZMK Next to a ZMK config

ZMK Next is a ZMK fork with an experimental Runtime Config service. You keep
your existing config repository, boards, shields, keymap, and normal build
workflow. The firmware exposes supported keymap changes to the local ZMK Next
Configurator over ZMK Studio's USB serial connection.

Adding ZMK Next takes two edits, one in `config/west.yml` and one in
`build.yaml`, plus a Studio unlock key in the keymap if it doesn't have one
yet (most don't). The guide pins the `v0.2-branch` release line: each rebuild picks up the
latest v0.2.x fixes, and nothing that changes the configurator protocol arrives
until you change the pin. Use a configurator release with the same major and
minor version (any v0.2.x configurator works with any v0.2.x firmware).

> ZMK Next is a personal project, shared as-is. There's no promise of support
> or long-term maintenance. To go back to upstream ZMK at any time, restore
> your original `zmk` entry in `config/west.yml`, remove `snippet: zmk-next`
> from `build.yaml`, and rebuild.

## Tested keyboards and limits

ZMK Next has been checked on two keyboards so far:

| Keyboard      | Board          | Checked how                                                         |
| ------------- | -------------- | ------------------------------------------------------------------- |
| Totem (split) | Seeed XIAO BLE | Built from this guide and used daily                                |
| Corne (split) | nice!nano      | Built with the snippet on every weekly release; not run on hardware |

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

If you try another keyboard, you're welcome to
[leave a keyboard report](https://github.com/rleyvasal/zmk-next/issues/new?template=keyboard-report.yml),
whether it worked or not. Reports show others what's been tried, but may not
get a reply.

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
- Studio is locked by default, so the keymap needs a `&studio_unlock` key
  (see [Add a Studio unlock key](#add-a-studio-unlock-key)).
- The `zmk-next` snippet puts Studio on the board's USB serial port, so the
  keyboard shows up as one serial port, also on boards such as the Seeed XIAO
  BLE that already have one. It turns off the UART console on that port. A board with a custom or
  conflicting USB CDC setup can instead keep its own Studio RPC snippet and add
  `cmake-args: -DCONFIG_ZMK_STUDIO=y -DCONFIG_ZMK_RUNTIME_CONFIG=y`.

### Add a Studio unlock key

Studio stays locked until you press a `&studio_unlock` key, so the
configurator can't change anything before then. Most keymaps don't have one,
including the stock keymaps that ship with ZMK. If yours already does, keep
it.

Otherwise, add a combo to your `.keymap`. It adds a key without changing any
existing binding:

```dts
/ {
    combos {
        compatible = "zmk,combos";
        combo_studio_unlock {
            key-positions = <0 1>;
            bindings = <&studio_unlock>;
        };
    };
};
```

Replace `0 1` with two keys you can press together. Positions count from 0 in
the order of your keymap's bindings, so `0 1` are the first two keys. If the
keymap already has a `combos` node, add `combo_studio_unlock` inside it
instead.

Don't wrap the unlock key in `#ifdef CONFIG_ZMK_STUDIO`: the snippet turns
Studio on in Kconfig, which the keymap can't see, so the key would be left out
and Studio would stay locked. To try ZMK Next without an unlock key, you can
add `-DCONFIG_ZMK_STUDIO_LOCKING=n` to the build's `cmake-args`. This lets any
program on the computer change the keymap over USB, so don't keep it.

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

On Linux, the browser can open the keyboard's serial port only if your user
may use serial devices. On most distributions that means the `dialout` group
(`uucp` on Arch):

```sh
sudo usermod -aG dialout "$USER"
```

Log out and back in for the change to apply. Until then, **Connect** shows no
keyboard or can't open it.

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

## 6. Optional: battery levels in the macOS menu bar

[ZMK Battery](https://github.com/rleyvasal/zmk-next-battery) shows both halves'
levels in the menu bar, for example `L 78% R 15%`, and notifies you at 20% and
10%. Download it from its
[Releases page](https://github.com/rleyvasal/zmk-next-battery/releases). It runs
on macOS 13 or later. The app isn't notarized, so the first launch needs
**Open Anyway** in System Settings › Privacy & Security.

- **Over Bluetooth** it reads the keyboard's Battery Service and needs no extra
  firmware options. On a split it also needs
  `CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_PROXY=y` (plus the fetching option
  from step 5) to show the peripheral half.
- **Over USB** it reads the same battery line as the configurator, so it needs
  step 5's zmk-studio-log setup.
- It treats the central half as the left one. On a keyboard whose right half
  is the central, the two levels show swapped.
- It finds a USB keyboard by ZMK's default USB IDs, so a build with its own
  USB vendor or product ID isn't found over USB.

## More ZMK Next options

ZMK Next adds a few options of its own, all off by default. They're documented
with the rest of ZMK's settings:

- [Battery](docs/config/battery.md): holding the level on USB, a charging
  estimate and charge state, and a settle time after unplugging.
- [Split](docs/config/split.md): a slower split interval while idle, keeping a
  disconnected half's last battery level, and swapping the battery services
  for host apps that expect the right half first.
- [System](docs/config/system.md): typing only over USB while the cable is in,
  and not advertising to computers over Bluetooth while on USB.

`CONFIG_ZMK_SPLIT_BLE_IDLE_INT` needs a Zephyr controller fix that ZMK's
Zephyr doesn't have yet. Without it, the split link can drop about a second
after the first key press that ends an idle period. The option's help text
names the Zephyr branch with the fix.

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
