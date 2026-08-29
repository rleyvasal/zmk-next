# Add ZMK Next to a ZMK config

ZMK Next is a ZMK fork with an experimental Runtime Config service. You keep
your existing config repository, boards, shields, keymap, and normal build
workflow. The firmware exposes supported keymap changes to the local ZMK Next
Configurator over ZMK Studio's USB serial connection.

This guide uses a pinned commit so an update cannot silently change the
firmware protocol. The revision below is the version currently tested with the
configurator.

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
      revision: 34ff30a32721719552fbdbbf6ce10929992675fd
      import: app/west.yml
  self:
    path: config
```

The imported ZMK Next manifest brings in the matching protocol messages module,
so your config does not need to list `zmk-next-messages` separately.

## 2. Enable Studio and Runtime Config

Add the Studio USB snippet and the two feature flags to the build that is
physically connected by USB:

```yaml
include:
  - board: your_board
    shield: your_shield
    snippet: studio-rpc-usb-uart
    cmake-args: -DCONFIG_ZMK_STUDIO=y -DCONFIG_ZMK_RUNTIME_CONFIG=y
```

For a split keyboard, enable them only on the central half. Build the peripheral
half from the same pinned revision, but do not add the snippet or flags to it:

```yaml
include:
  - board: your_board
    shield: your_keyboard_left
    snippet: studio-rpc-usb-uart
    cmake-args: -DCONFIG_ZMK_STUDIO=y -DCONFIG_ZMK_RUNTIME_CONFIG=y
  - board: your_board
    shield: your_keyboard_right
```

Replace the board and shield names with the values already used by your config.
If the right half is your central half, put the flags and snippet there instead.

### Studio prerequisites

These are standard ZMK Studio requirements and normally need to be handled only
once:

- The keyboard needs a physical layout with a `keys` property. Most keyboards
  already prepared for ZMK Studio have one.
- Studio is locked by default. Keep your existing `&studio_unlock` key/combo,
  or temporarily add `-DCONFIG_ZMK_STUDIO_LOCKING=n` while evaluating ZMK Next.
- The generic `studio-rpc-usb-uart` snippet works for most boards. A board with
  a custom or conflicting USB CDC setup may need a board-specific Studio RPC
  snippet, as it would with upstream ZMK Studio.

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

Download a configurator archive from the project's GitHub Releases page,
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

ZMK Next is experimental. Update the pinned `revision` deliberately, rebuild
both halves, and use the configurator release documented for that revision.
Keeping the firmware revision pinned makes rollback as simple as restoring the
previous SHA and rebuilding.
