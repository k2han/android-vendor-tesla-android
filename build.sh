#!/bin/bash
# Rebuilds the userspace pieces into prebuilt/. Run on an x86_64 Linux host.
#   NDK=~/build/ndk/android-ndk-r27c   (Android NDK r27c)
#   aarch64-linux-gnu-gcc              (Ubuntu gcc-aarch64-linux-gnu)
# The kernel module is built separately, see README.md.
set -eu
cd "$(dirname "$0")"
NDK=${NDK:-$HOME/build/ndk/android-ndk-r27c}
BIN=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
T=src/tinyalsa
TA="$T/src/pcm.c $T/src/pcm_hw.c $T/src/pcm_plugin.c $T/src/snd_card_plugin.c $T/src/limits.c"
mkdir -p prebuilt

# audio HAL: the Pi runs the 32-bit HAL (/vendor/lib/hw), arm64 copy built for completeness
for t in armv7a-linux-androideabi34 aarch64-linux-android34; do
  $BIN/$t-clang -O2 -fPIC -shared -Wl,--no-undefined -Ihal/inc -I$T/include \
    hal/audio_hw_tamic.c $TA -llog -ldl -o prebuilt/audio.primary.default.$t.so
done
cp prebuilt/audio.primary.default.armv7a-linux-androideabi34.so prebuilt/audio.primary.default.so

# WebSocket -> Loopback bridge (static, no Android libs needed)
aarch64-linux-gnu-gcc -O2 -static -I$T/include src/ta-mic-bridge.c $TA -ldl -o prebuilt/ta-mic-bridge 2>/dev/null

cp src/mic-capture.js prebuilt/
sha256sum prebuilt/*
