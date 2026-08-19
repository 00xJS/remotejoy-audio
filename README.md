# RemoteJoy Audio — PSP gameplay video **and audio** over USB, on macOS

Streams a real PSP's screen **and its game audio** to a computer over the USB cable, and runs
natively on Apple Silicon Macs.

The audio half has not existed before. Every previous PSP screen-streaming tool — TyRaNiD's
original RemoteJoy, RemoteJoyLite, and the modern USB Video Class plugin — carried **video only**.
The standard advice was always to run a 3.5&nbsp;mm cable from the headphone jack into a capture
input. This project removes that cable: game audio is captured inside the PSP and streamed over the
same USB connection as the video.

Tested on a PSP running 6.61 PRO-C with macOS on Apple Silicon.

---

## How it works

```
PSP (kernel plugin)                         Mac (libretro core in RetroArch)
────────────────────                        ───────────────────────────────
sceDisplaySetFrameBuf ──hook──┐
                              ├─► 480x272 framebuffer ─┐
sceAudioOutputBlocking  ──┐   │                        │
sceAudioOutputPanned…   ──┼─hook──► 8-channel mixer ───┤ USB bulk ─► demux ─┬─► video
sceAudioSRCOutput…      ──┘        44.1kHz stereo s16  ┘                     └─► audio
```

**Audio capture.** The plugin hooks the `sceAudio` output calls through the PSP's syscall table.
Each hooked call runs the original function first — so the firmware's own pointer validation
happens before we touch anything — then copies the submitted PCM.

**Mixing.** The PSP mixes up to 8 hardware channels in hardware, so capturing a single channel
yields only part of the soundtrack (music but no effects, typically). Every active channel is
summed onto one timeline indexed by absolute frame number, with a short holding window so channels
that submit slightly out of step still land in the same stretch. Saturating addition prevents
wraparound distortion on loud passages.

**Sample counts without reservation.** The hooks install after a game has already reserved its
audio channels, so `sceAudioChReserve` is never observed. The frame count is taken from each
blocking call's return value (the queued sample count) instead, which also prevents over-reading a
game's buffer if a cached length goes stale.

**Bandwidth.** 44.1kHz stereo 16-bit is ~176&nbsp;KB/s, under 3% of what the video already uses on
the PSP's Hi-Speed USB bus.

**Clean exit.** Returning to the XMB does not unload plugins — the PSP reboots its kernel. The
plugin exports `module_reboot_before` / `module_reboot_phase` so it can stop capturing, release its
VBlank interrupt vector, halt its threads and restore the patched syscall entries before the kernel
goes down. Without that, the console hangs on exit.

---

## Install

### PSP (custom firmware required)

1. Copy `prebuilt/RemoteJoyLite.prx` to `ms0:/seplugins/RemoteJoyLite.prx`
   (PSP Go: `ef0:/seplugins/`, and use `ef0:` below).
2. Add this line to `ms0:/seplugins/game.txt` (and `vsh.txt` / `pops.txt` if you also want the XMB
   and PS1 games):

   ```
   ms0:/seplugins/RemoteJoyLite.prx 1
   ```
3. Reboot the PSP.

### Mac

No driver is needed — macOS lets a userspace process claim the PSP's vendor-specific USB interface
directly, which is why the Windows-only Zadig/libusb-win32 dance has no equivalent here.

```bash
brew install --cask retroarch-metal      # must be the Metal build on Apple Silicon
git clone https://github.com/libretro/libretro-remotejoy
cd libretro-remotejoy
git apply /path/to/client/libretro-remotejoy-audio.patch
make platform=osx
```

Then install the core with `scripts/install-core.sh`. That script deletes the old core before
copying rather than overwriting it — overwriting a dylib in place leaves macOS holding stale
code-signed pages, and the next launch is killed with `Code Signature Invalid`.

Start a game on the PSP, connect USB, then run `scripts/stream.sh`.

### Before you return to the XMB

**Unplug the USB cable before quitting a game back to the home screen.** With the cable still
attached the PSP can hang on the way out and need a battery pull. Leaving a game while the plugin is
actively streaming asks the console to tear down its audio, display and USB drivers while a kernel
plugin is still feeding all three. Unplug first, exit, then reconnect for the next game.

---

## Building the plugin

Needs the [pspdev SDK](https://github.com/pspdev/pspdev) (ships native Apple Silicon builds):

```bash
git clone https://github.com/PSP-Archive/RemoteJoyLite
cd RemoteJoyLite
git apply /path/to/psp-plugin/remotejoylite-audio.patch
cp /path/to/psp-plugin/hook_audio.{c,h} RemoteJoyLite_psp/
cd RemoteJoyLite_psp && make
```

Building the 2012 source with a modern toolchain needs two fixes, both included in the patch: an
obsolete pointer cast in `BuildFrame`, and the firmware-versioned kernel stub libraries
(`-lpspsysmem_kernel_660 -lpsploadcore_kernel_660`).

> Do **not** statically link PRO CFW's `libpspsystemctrl_kernel` here. An unresolved kernel import
> stops the entire module from loading, so the plugin silently never runs.

---

## Diagnostics

The core logs a counter line every couple of seconds:

```
[audio] hooks=0x3ff chres=0 srcres=0 out=0 panned=53208 src=0 drop=0 chans=0x2c rate=44100 ring=8192 badptr=0 disp=17524 slots=10
```

| Field | Meaning |
|---|---|
| `hooks` | bitmask of installed hooks; `0x3ff` = all ten |
| `out` / `panned` / `src` | calls intercepted per audio API — shows which one a game uses |
| `chans` | bitmask of channels being mixed |
| `drop` / `badptr` | dropped bytes / buffers rejected by the address check (both should stay 0) |
| `disp` | display-hook entries; a control proving syscall hooks fire in game context |

---

## Limitations

- Some games don't stream video (they drive the GPU in ways the display hook misses), and UMD
  video playback isn't capturable. Inherited from RemoteJoyLite.
- Audio from the XMB itself is not captured — the menu uses a different path than games.
- Only 44.1kHz sources are correct today; other rates are not resampled yet.
- Input forwarding (playing from the computer) is still unimplemented in the libretro client.
- Exiting to the XMB with the USB cable connected can hang the console (see above) - unplug first.
- Kernel-mode code: a bug can hang the PSP. A battery pull recovers it, and custom firmware is not
  at risk. Hold **R** while powering on to reach recovery and disable plugins.

## Credits and licensing

Built on the work of others:

- **RemoteJoy / PSPLINK** — TyRaNiD (James F), BSD licensed
- **RemoteJoyLite** — the PSP-side plugin this extends, [PSP-Archive/RemoteJoyLite](https://github.com/PSP-Archive/RemoteJoyLite), mixed licensing per upstream
- **libretro-remotejoy** — the cross-platform client, [libretro/libretro-remotejoy](https://github.com/libretro/libretro-remotejoy), **GPLv2**
- **uofw** — the reverse-engineered firmware documentation that made the syscall and reboot behaviour knowable

Changes here are distributed as patches against those upstreams and carry their respective
licenses; the client patch is GPLv2. `hook_audio.c` / `hook_audio.h` are new work and are released
under the same terms as the plugin they extend.
