# ZMK Next

ZMK Next is a firmware runtime-configuration platform built on ZMK's hardware,
transport, and behavior foundations. It makes supported behavior *instances*
live-configurable while retaining compiled firmware engines and fixed resource
budgets.

The protocol contract lives in `zmk-next-messages`; the editor and device client
live in `zmk-next-configurator`. This repository owns firmware validation,
runtime dispatch, safe activation, and persistent configuration generations.

The initial architecture is documented in
[`docs/architecture/runtime-config.md`](docs/architecture/runtime-config.md).
