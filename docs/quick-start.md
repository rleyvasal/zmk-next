# Add ZMK Next to a ZMK config

ZMK Next is a ZMK fork with an experimental Runtime Config service. You keep
your existing config repository, boards, shields, keymap, and normal build
workflow. The firmware exposes supported keymap changes to the local ZMK Next
Configurator over ZMK Studio's USB serial connection.

Adding ZMK Next takes two edits: one in `config/west.yml` and one in
`build.yaml`. The guide pins a release tag so an update cannot silently change
the firmware protocol. Use a configurator release with the same major and minor
version (any v0.2.x configurator works with any v0.2.x firmware).

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
      revision: v0.2.0
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
- The `zmk-next` snippet uses the same USB serial setup as upstream's
  `studio-rpc-usb-uart` and works for most boards. A board with a custom or
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
Serial connection used by **Connect**.

After connecting, make a small key change and confirm that it types immediately.
Use **Restore stock configuration** to remove the runtime overlay and return to
the compiled keymap.

## Updating later

ZMK Next merges upstream ZMK every week and tags a patch release (for example
v0.2.1) when the merge passes its tests. Patch releases never change the
configurator protocol. A new minor version (v0.3.0) means a new configurator
release is needed too.

Update the pinned `revision` to a newer tag deliberately and rebuild both
halves. Rolling back is restoring the previous tag and rebuilding.
