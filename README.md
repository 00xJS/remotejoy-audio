# RemoteJoy Audio — PSP gameplay video **and audio** over USB, on macOS

Streams a real PSP's screen **and its game audio** to a computer over the USB cable, natively on
Apple Silicon Macs.

The audio half is new. Every previous PSP screen-streaming tool — TyRaNiD's original RemoteJoy,
RemoteJoyLite, and the modern USB Video Class plugin — carried **video only**, and the standard
advice was to run a 3.5&nbsp;mm cable from the headphone jack into a capture input. This project
captures game audio inside the PSP and streams it over the same USB connection as the video, so a
single cable carries both.

Developed and tested on a PSP running **6.61 PRO-C** with macOS on Apple Silicon.

> **Status:** the audio path works and sounds clean. One known issue is under active tuning — see
> [Known issues](#known-issues). This is a working project, not a finished 1.0.

---

## How it works

```
PSP (kernel plugin)                         Mac (libretro core in RetroArch)
────────────────────                        ───────────────────────────────
sceDisplaySetFrameBuf ──hook──┐
                              ├─► 480x272 framebuffer ─┐
sceAudioOutputBlocking  ──┐   │                        │
sceAudioOutputPanned…   ──┼─hook──► 8-channel mixer ───┤ USB bulk ─► demux ─┬─► video
sceAudioSRCOutput…      ──┘        44.1kHz stereo s16  ┘                     └─► audio ─► DRC ─► out
```

**Audio capture.** The plugin hooks the `sceAudio` output calls through the PSP's syscall table,
running each original first (so the firmware's own pointer validation applies) and then copying the
submitted PCM.

**Mixing with correct levels.** The PSP mixes up to 8 hardware channels, so capturing one channel
gives only part of the soundtrack. Every active channel is summed onto a single timeline indexed by
real playback time (so channels that started at different moments stay aligned), each scaled by the
per-channel volume the game requested, with saturating addition to prevent overload distortion.

**Mono/stereo detection.** Because the plugin loads after the game has already reserved its audio
channels, it never sees `sceAudioChReserve` and so cannot be told each channel's format. It recovers
the format directly from the audio driver's channel table, located **without any hardcoded address**:
it disassembles the resolved `sceAudio` functions to find the driver's global pointers, then *proves*
a candidate is the real table by checking that the volumes stored there match the volumes just passed
to that channel (a 32-bit match). Every read is range-checked to valid kernel RAM; on any doubt it
falls back to assuming stereo, so a wrong read can never freeze the console.

**Playback rate control.** RetroArch calls the core at the video frame rate, but the PSP produces
audio at a steady 44.1&nbsp;kHz. The client keeps a small buffer and adjusts consumption with an
EMA-smoothed controller, so audio stays smooth even when the video rate dips, without pitch wobble.

**Clean exit.** Returning to the XMB reboots the PSP kernel rather than unloading plugins, so the
plugin exports `module_reboot_before`/`module_reboot_phase` to stop capturing, release its interrupt
vector, halt its threads and restore the patched syscall entries before the kernel goes down.

---

## Install

### PSP (custom firmware required)

1. Copy `prebuilt/RemoteJoyLite.prx` to `ms0:/seplugins/RemoteJoyLite.prx`
   (PSP Go: `ef0:/seplugins/`, and use `ef0:` below), or run `scripts/install-plugin.sh` with the
   PSP mounted in USB mode.
2. Enable it — add this line to `ms0:/seplugins/game.txt` (and `vsh.txt` / `pops.txt` for the XMB
   and PS1 games):

   ```
   ms0:/seplugins/RemoteJoyLite.prx 1
   ```
3. Reboot the PSP.

### Mac

No driver is needed — macOS lets a userspace process claim the PSP's vendor-specific USB interface
directly, so there is no Windows-style Zadig/libusb-win32 step.

```bash
brew install --cask retroarch-metal      # must be the Metal build on Apple Silicon
git clone https://github.com/libretro/libretro-remotejoy
cd libretro-remotejoy
git apply /path/to/client/libretro-remotejoy-audio.patch
make platform=osx
```

Install the built core with `scripts/install-core.sh` (it deletes the old core before copying, so
macOS doesn't hold a stale code-signed page and kill RetroArch on launch).

Start a game on the PSP, connect USB, then run `scripts/stream.sh` (it launches RetroArch straight
into the core and logs to `~/rj-log.txt`).

> **Unplug the USB cable before returning to the XMB.** Leaving a game while streaming asks the PSP
> to tear down its audio, display and USB drivers at once under a live plugin, and it can hang. If it
> ever does, or you see repeated `Error: -7`, do a full power-cycle (not a soft reboot).

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

The patch also carries the two fixes needed to build the 2012 source with a modern toolchain (an
obsolete pointer cast, and the `_660` firmware kernel stub libraries).

> Do **not** statically link PRO CFW's `libpspsystemctrl_kernel`. An unresolved kernel import stops
> the whole module from loading, so the plugin silently never runs.

---

## Diagnostics

`scripts/stream.sh` logs to `~/rj-log.txt`. Two lines matter:

```
[audio] hooks=0x3ff panned=53208 drop=0 ring=1024 disp=… clip=0 mono=0x80000020 slots=10
[rate]  video=58.0fps audio=64.0 pkt/s
```

| Field | Meaning |
|---|---|
| `hooks` | bitmask of installed hooks; `0x3ff` = all ten |
| `panned`/`out`/`src` | calls intercepted per audio API — shows which one a game uses |
| `drop`/`clip`/`badptr` | dropped bytes / saturated samples / rejected buffers — all should stay 0 |
| `mono` | bit 31 set = channel-format lock succeeded; bits 0–7 = which channels are mono |
| `[rate] video` | objective client-measured video frame rate |

Set `RJ_NOAUDIO=1` before `stream.sh` to run the plugin with audio capture inert — useful for
isolating audio-related effects on the video frame rate.

---

## Known issues

- **Frame rate under load (under investigation).** With audio streaming, the game's own render rate
  can drop on some titles. The client build logs `[rate] video=…fps` and honours `RJ_NOAUDIO=1` so
  the cause can be isolated; a lighter transfer mode (frame-skip / 16-bit) is the planned mitigation.
- **XMB audio isn't captured** — the menu uses a different audio path than games.
- **Only 44.1&nbsp;kHz sources are correct** — other rates are not resampled yet.
- Some games don't stream video, and UMD video playback isn't capturable (inherited from RemoteJoyLite).
- Input forwarding (playing from the computer) is unimplemented in the libretro client.
- **Kernel-mode code:** a bug can hang the PSP. A battery pull recovers it and custom firmware is not
  at risk; hold **R** at power-on to reach recovery and disable plugins.

## Credits and licensing

Built on the work of others:

- **RemoteJoy / PSPLINK** — TyRaNiD (James F), BSD licensed
- **RemoteJoyLite** — the PSP-side plugin this extends, [PSP-Archive/RemoteJoyLite](https://github.com/PSP-Archive/RemoteJoyLite), mixed licensing per upstream
- **libretro-remotejoy** — the cross-platform client, [libretro/libretro-remotejoy](https://github.com/libretro/libretro-remotejoy), **GPLv2**
- **uofw** — the reverse-engineered firmware documentation that made the syscall, audio and reboot behaviour knowable

Changes here are distributed as patches against those upstreams and carry their respective licenses;
the client patch is GPLv2. `hook_audio.c` / `hook_audio.h` are new work, released under the same
terms as the plugin they extend.
