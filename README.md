# Tesla cabin mic -> Tesla Android (prototype)

Forwards the Tesla browser microphone into Android as a regular capture device, so apps
on the Pi (OpenClaw Talk, assistants) can record the cabin mic.

```
Tesla browser (getUserMedia, AEC/NS/AGC)
  -> mic-capture.js, s16le mono 48 kHz, 20 ms frames
  -> wss://device.teslaandroid.com/sockets/mic
  -> lighttpd proxy -> 127.0.0.1:9997 ta-mic-bridge
  -> ALSA Loopback card, device 0 (playback)
  -> Loopback device 1 (capture) <- replacement audio.primary.default.so (in_read)
  -> AudioFlinger -> apps
```

Root cause found on the stock build: `/vendor/lib/hw/audio.primary.default.so` is the AOSP
stub HAL, its input always returns silence. `hal/audio_hw_tamic.c` is that stub with
`in_read` reading the Loopback capture through tinyalsa (mono/stereo conversion, linear
resample, reopen every 1 s on failure, silence meanwhile). Output is unchanged.

Status: browser -> bridge -> Loopback verified on the car (tinycap shows cabin audio).
Replacement HAL built, not yet verified on the device.

## Layout

| Path | What |
|---|---|
| `src/ta-mic-bridge.c` | WebSocket server + tinyalsa writer |
| `src/mic-capture.js` | browser capture, inlined into the web UI `index.html` |
| `src/tinyalsa/` | tinyalsa sources (BSD), https://github.com/tinyalsa/tinyalsa.git @ a7cd26a |
| `hal/audio_hw_tamic.c` | replacement primary audio HAL (stub HAL + Loopback input) |
| `hal/audio_hw_stub.c` | the AOSP stub it was derived from, for diffing |
| `hal/inc/` | minimal AOSP headers needed to build the HAL with the NDK |
| `deploy.sh` | non-persistent deploy over adb root (bind mounts, reboot restores stock) |
| `build.sh` | rebuilds HAL and bridge into `prebuilt/` |
| `prebuilt/` | binaries, see sha256 below |
| `kernel/` | Pi kernel config and modinfo used for `snd-aloop.ko` |

## Device

`tesla_android_rpi4-userdebug 14 UPM1.231025.063`, kernel `6.1.84-android14-0-GloDroid-g7104721dd81f`,
SELinux permissive, adb root.

## Build

Userspace: `./build.sh` (Android NDK r27c, `aarch64-linux-gnu-gcc`).

Kernel module `snd-aloop.ko` (`CONFIG_SND_ALOOP=m`):
- Source: `https://github.com/glodroid/glodroid_forks.git`, branch `android-14.0`, commit `fd2593628`
- Tesla Android kernel patches applied (`patches-aosp/glodroid/kernel/broadcom/0001..0011` from tesla-android/android-raspberry-pi)
- Config: `kernel/kernel.config` (taken from the Pi `/proc/config.gz`) plus `CONFIG_SND_ALOOP=m`
- `make ARCH=arm64 LLVM=1 O=out olddefconfig && make ... modules_prepare && make ... M=sound/drivers modules`
- vermagic must match: `6.1.84-android14-0-GloDroid-g7104721dd81f SMP preempt mod_unload modversions aarch64`

## Deploy (test only)

Copy `deploy.sh` and `prebuilt/*` to `/root/tamic/` on a host with adb to the Pi, then
`deploy.sh up|status|down`. Everything is bind-mounted, a Pi reboot restores stock.
