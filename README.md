# ZMK Next

ZMK Next is a firmware runtime-configuration platform built on ZMK's hardware,
transport, and behavior foundations. It makes supported behavior _instances_
live-configurable while retaining compiled firmware engines and fixed resource
budgets.

The protocol contract lives in `zmk-next-messages`; the editor and device client
live in `zmk-next-configurator`. This repository owns firmware validation,
runtime dispatch, safe activation, and persistent configuration generations.

The initial architecture is documented in
[`docs/architecture/runtime-config.md`](docs/architecture/runtime-config.md).

## Try it on a keyboard

Your existing config repo needs two edits, plus an unlock key in most keymaps:

1. Point the `zmk` project in `config/west.yml` at a ZMK Next release line
   (`v0.2-branch`).
2. Add `snippet: zmk-next` to the keyboard's USB/central build in `build.yaml`.
3. If the keymap has no `&studio_unlock` key, add one. Most stock keymaps
   don't; the guide has a combo to paste in.
4. Build and flash the firmware as usual.
5. Run the downloadable ZMK Next Configurator locally and connect in a
   Web Serial-capable browser.

Tried it on your keyboard? [Report how it went](https://github.com/rleyvasal/zmk-next/issues/new?template=keyboard-report.yml),
whether it worked or not.

See the [generic integration guide](docs/quick-start.md) for copy-and-paste
examples for both single-piece and split keyboards, plus the few ZMK Studio
prerequisites that firmware cannot infer automatically. So far it has been
checked on the Totem and the Corne; the guide lists what other keyboards need.

## Companion projects

- [ZMK Next Configurator](https://github.com/rleyvasal/zmk-next-configurator):
  the local keymap editor that connects to the keyboard over USB.
- [zmk-studio-log](https://github.com/rleyvasal/zmk-studio-log): a module that
  streams the keyboard's log and battery levels to the configurator.
- [ZMK Battery](https://github.com/rleyvasal/zmk-next-battery): a macOS menu
  bar app showing both halves' battery, over Bluetooth or USB.
- [zmk-next-messages](https://github.com/rleyvasal/zmk-next-messages): the
  protocol shared by the firmware and the configurator.

ZMK Next is based on [ZMK Firmware](https://zmk.dev/), the MIT-licensed keyboard
firmware built on the [Zephyr Project](https://www.zephyrproject.org/) RTOS. The
upstream documentation and community remain the reference for its compiled
hardware, transport, and behavior foundations.
