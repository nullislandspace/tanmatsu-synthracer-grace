## Race The Synth

Tanmatsu game "Race The Synth".

This uses [Graceloader](https://github.com/nullislandspace/tanmatsu-graceloader) as a launcher.

Built on **SynthEngine3D**, a reusable engine extracted from this game (run
loop, software 3D renderer, audio mixer + procedural music, menus, input
bindings, save framework). It now lives in its own repository,
[nullislandspace/synthengine3D](https://github.com/nullislandspace/synthengine3D),
and is consumed here as a git submodule at [`synthengine3D/`](synthengine3D/) --
a dual-mode IDF / plain-CMake component with its own docs; the game itself is
the content + rules in [`main/`](main/).

Clone with submodules, or the build will fail with a missing `synthengine3D/`:

```sh
git clone --recursive git@github.com:nullislandspace/tanmatsu-synthracer-grace.git
# or, in an existing clone:
git submodule update --init
```

## License

This software is under the [MIT license](https://opensource.org/license/mit). The MIT license allows others to build upon your work without restrictions while also making sure you retain your attribution.

(C) 2026 Rene Schickbauer

