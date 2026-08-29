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

The shortest integration path is:

1. Point the `zmk` project in your config repo's `config/west.yml` at a pinned
   ZMK Next revision.
2. Enable `CONFIG_ZMK_STUDIO` and `CONFIG_ZMK_RUNTIME_CONFIG` on the keyboard's
   USB/central build, and add the Studio USB transport snippet.
3. Build and flash the firmware as usual.
4. Run the downloadable ZMK Next Configurator locally and connect in a
   Web Serial-capable browser.

See the [generic integration guide](docs/quick-start.md) for copy-and-paste
examples for both single-piece and split keyboards, plus the few ZMK Studio
prerequisites that firmware cannot infer automatically.

ZMK Next is based on [ZMK Firmware](https://zmk.dev/), the MIT-licensed keyboard
firmware built on the [Zephyr Project](https://www.zephyrproject.org/) RTOS. The
upstream documentation and community remain the reference for its compiled
hardware, transport, and behavior foundations.
