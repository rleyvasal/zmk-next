# Support for ZMK Next

ZMK Next is a personal project, built for my own keyboard and shared as-is.
There's no promise of support, fixes or long-term maintenance. Issues and pull
requests are welcome, but may not get an answer.

ZMK Next is also a fork of ZMK. Upstream ZMK's Discord and issue tracker support
ZMK itself, not this fork, so please don't take ZMK Next problems there.

- **Setting it up:** follow the
  [quick start](https://github.com/rleyvasal/zmk-next/blob/main/docs/quick-start.md).
- **Tried it on a keyboard:** you're welcome to
  [leave a keyboard report](https://github.com/rleyvasal/zmk-next/issues/new?template=keyboard-report.yml),
  whether it worked or not. It shows others what's been tried.
- **Something broke:** you can [open an issue](https://github.com/rleyvasal/zmk-next/issues/new)
  with your board, shield, `config/west.yml` and the error. To get going again
  right away, go back to upstream ZMK: restore your original `zmk` entry in
  `config/west.yml`, remove `snippet: zmk-next` from `build.yaml`, and rebuild.
- **Configurator, log module or battery app:** issues go in
  [zmk-next-configurator](https://github.com/rleyvasal/zmk-next-configurator/issues),
  [zmk-studio-log](https://github.com/rleyvasal/zmk-studio-log/issues) or
  [zmk-next-battery](https://github.com/rleyvasal/zmk-next-battery/issues).

Keymaps, behaviors, boards and shields work as in upstream ZMK, so the
[ZMK documentation](https://zmk.dev/docs) applies to them.
