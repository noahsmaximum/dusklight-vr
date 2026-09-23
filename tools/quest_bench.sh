#!/usr/bin/env bash
# Quest benchmark: boots straight into a stage with extra cvars, then averages the runtime's frame
# rate and app GPU time over the last 10 VrApi lines. Usage: tools/quest_bench.sh "<label>" [--cvar k=v ...]
set -u
export PATH="/f/Android/sdk/platform-tools:$PATH" MSYS_NO_PATHCONV=1
label=$1; shift
pkg=dev.twilitrealm.dusk.vr
adb shell am force-stop $pkg; adb logcat -c
adb shell "am start -n $pkg/dev.twilitrealm.dusk.DuskActivity --es dusk_args '--dvd /storage/emulated/0/Download/tp-linkle.iso --mods /storage/emulated/0/Download/dusk-mods --stage ${STAGE:-F_SP103,0,5,-1} $*'" >/dev/null
for _ in $(seq 1 ${WAIT:-9}); do adb shell "sleep 5"; done
adb logcat -d | grep "VrApi.*FPS=" | tail -10 | sed -E 's/.*FPS=([0-9]+)\/[0-9]+.*,App=([0-9.]+)ms.*/\1 \2/' |
    awk -v l="$label" '{f+=$1; a+=$2; n++} END {if (n) printf "%-28s fps %5.1f  app GPU %5.1f ms  (%d samples)\n", l, f/n, a/n, n; else print l ": no samples"}'
