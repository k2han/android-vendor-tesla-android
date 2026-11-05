#!/bin/bash
# Runs ON LXC112 (192.168.10.27). Non-persistent test deploy of the browser-mic path to the Pi.
# usage: deploy.sh [up|down|status]   (files expected in /root/tamic/)
# Everything is bind-mounted over the read-only partitions; a Pi reboot restores stock.
set -u
D=100.73.28.77:5555
A="adb -s $D"
L=/root/tamic
R=/data/local/tmp/tamic
mode=${1:-up}
adb connect $D >/dev/null 2>&1
$A root >/dev/null 2>&1; sleep 2; adb connect $D >/dev/null 2>&1
[ "$($A shell id -u 2>/dev/null | tr -d '\r')" = 0 ] || { echo "no root adb on $D"; exit 1; }

sh_() { $A shell "$@" 2>&1 | tr -d '\r'; }

if [ "$mode" = status ]; then
  sh_ 'cat /proc/asound/cards; lsmod | grep aloop; ps -A | grep -E "ta-mic|lighttpd|audio"; mount | grep tamic; tail -20 /data/local/tmp/tamic/bridge.log 2>/dev/null'
  exit 0
fi

if [ "$mode" = down ]; then
  sh_ "pkill -f ta-mic-bridge; for m in /vendor/lib/hw/audio.primary.default.so /vendor/etc/audio.rpi4.xml /vendor/tesla-android/lighttpd/lighttpd.conf /vendor/tesla-android/lighttpd/www-default/index.html; do while mount | grep -q \" \$m \"; do umount \$m; done; done; rmmod snd_aloop 2>/dev/null; setprop ctl.restart lighttpd; pkill -f android.hardware.audio.service; pkill -x audioserver; echo rolled back"
  exit 0
fi

# ---- up ----
sh_ "mkdir -p $R/ov"
for f in snd-aloop.ko ta-mic-bridge mic-capture.js audio.primary.default.so; do $A push $L/$f $R/$f >/dev/null || { echo "push $f failed"; exit 1; }; done
sh_ "chmod 755 $R/ta-mic-bridge"

# 1) kernel loopback card
sh_ "grep -q Loopback /proc/asound/cards || insmod $R/snd-aloop.ko pcm_substreams=1 id=Loopback"
sh_ "cat /proc/asound/cards" | grep -q Loopback || { echo "insmod failed:"; sh_ "dmesg | tail -5"; exit 1; }

# 2) audio HAL input -> Loopback capture (device 1)
X=$(sh_ "ls /vendor/etc/audio.rpi4.xml /vendor/etc/audio/audio.rpi4.xml 2>/dev/null | head -1")
[ -n "$X" ] || X=$(sh_ "grep -rl 'cardname=\"Dummy\"' /vendor/etc 2>/dev/null | head -1")
[ -n "$X" ] || { echo "audio xml not found"; exit 1; }
# unmount first: sed must read the stock file, never our own overlay (reading the overlay truncated it once)
sh_ "while mount | grep -q ' $X '; do umount $X; done; sed 's/cardname=\"Dummy\" device=\"0\"/cardname=\"Loopback\" device=\"1\"/' $X > $R/ov/audio.xml && grep -q Loopback $R/ov/audio.xml && mount --bind $R/ov/audio.xml $X"

# 2b) replacement audio HAL (stock one is the AOSP stub that returns silence for the mic)
H=/vendor/lib/hw/audio.primary.default.so
sh_ "while mount | grep -q ' $H '; do umount $H; done; chmod 644 $R/audio.primary.default.so; chcon \$(ls -Z $H | awk '{print \$1}') $R/audio.primary.default.so; mount --bind $R/audio.primary.default.so $H; ls -lZ $H"

# 3) lighttpd: proxy /sockets/mic -> 127.0.0.1:9997 (inserted before the /sockets/audio block)
C=/vendor/tesla-android/lighttpd/lighttpd.conf
[ -n "$C" ] || C=$(sh_ "grep -rl 'sockets/audio' /vendor/etc 2>/dev/null | head -1")
if [ -n "$C" ]; then
  sh_ "while mount | grep -q ' $C '; do umount $C; done"
  $A pull $C /tmp/lighttpd.conf.orig >/dev/null
  python3 - <<'PY'
s=open('/tmp/lighttpd.conf.orig').read()
blk='    $HTTP["url"] =~ "^/sockets/mic" {\n        proxy.server  = ( "" => (( "host" => "127.0.0.1", "port" => 9997 )))\n        proxy.header = ( "upgrade" => "enable" )\n    }\n'
if '/sockets/mic' not in s:
    i=s.find('    $HTTP["url"] =~ "^/sockets/audio"')
    s=s[:i]+blk+s[i:] if i>=0 else s
open('/tmp/lighttpd.conf.new','w').write(s)
PY
  $A push /tmp/lighttpd.conf.new $R/ov/lighttpd.conf >/dev/null
  sh_ "mount --bind $R/ov/lighttpd.conf $C"
else echo "WARN lighttpd.conf not found"; fi

# 4) inject mic-capture.js into the stable web UI
W=/vendor/tesla-android/lighttpd/www-default
if [ -n "$W" ] && [ "$W" != . ]; then
  # inline the script (the web root is read-only, so no new file can be added there)
  sh_ "while mount | grep -q ' $W/index.html '; do umount $W/index.html; done; cp $W/index.html $R/ov/index.html; { printf '<script>\n'; cat $R/mic-capture.js; printf '</script>\n'; } > $R/ov/inl; sed -i '/<script src=\"audioplayback.js\"><\/script>/r $R/ov/inl' $R/ov/index.html; grep -c __taMic $R/ov/index.html; mount --bind $R/ov/index.html $W/index.html"
else echo "WARN web root not found"; fi

# 5) restart services, start bridge
sh_ "setprop ctl.restart lighttpd; pkill -f android.hardware.audio.service; pkill -x audioserver; sleep 3"
$A shell "pkill -f ta-mic-bridge; nohup setsid $R/ta-mic-bridge 9997 ${TAMIC_CH:-2} >$R/bridge.log 2>&1 </dev/null & sleep 1"
sleep 2
sh_ "cat /proc/asound/cards; mount | grep tamic; tail -3 $R/bridge.log"
echo "UP. web root=$W xml=$X conf=$C"
