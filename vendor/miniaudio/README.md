# miniaudio

[miniaudio](https://miniaud.io) 0.11.25, an unmodified copy of `miniaudio.h`
from https://github.com/mackron/miniaudio at tag `0.11.25` (git blob
`c6d493ee81f553b8dd584e402453f130c20e0546`). Public domain or MIT-0, at your
choice; the license text is at the end of `miniaudio.h`.

magpie_tui plays its sound effects through it (`tui/sound.c`): Core Audio on
macOS, WASAPI on Windows, and PipeWire, PulseAudio or ALSA on Linux, each
loaded at run time, so building needs no audio libraries.

- `miniaudio_tui.h`: the options magpie_tui builds it with (only the
  low-level playback device API). Include it instead of `miniaudio.h`.
- `miniaudio.c`: the implementation, compiled once.

To update, replace `miniaudio.h` with the new release's and update the version
and blob above.
