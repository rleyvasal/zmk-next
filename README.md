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

ZMK Next is based on [ZMK Firmware](https://zmk.dev/), the MIT-licensed keyboard
firmware built on the [Zephyr Project](https://www.zephyrproject.org/) RTOS. The
upstream documentation and community remain the reference for its compiled
hardware, transport, and behavior foundations.
