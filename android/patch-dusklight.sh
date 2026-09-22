#!/usr/bin/env bash
# Turns a Dusklight checkout into the "VR edition" the mod needs on Android.
#
# Two changes, both additive:
#   1. AndroidManifest.xml declares the app as an immersive OpenXR app, so Quest/Pico launch it in
#      VR instead of as a flat panel, and so the OpenXR loader can find the runtime broker.
#   2. Aurora requests Dawn's shared-texture/shared-fence features when the GPU offers them. That is
#      what lets a mod hand Dawn's rendered frames to OpenXR through Dawn's public API instead of
#      reaching into its internals (impossible on Android: the release build exports neither).
#
# Usage: android/patch-dusklight.sh <dusklight checkout>
set -euo pipefail

root="${1:?usage: patch-dusklight.sh <dusklight checkout>}"
manifest="$root/platforms/android/app/src/main/AndroidManifest.xml"
gpu="$root/extern/aurora/lib/webgpu/gpu.cpp"

for f in "$manifest" "$gpu"; do
    [[ -f "$f" ]] || { echo "not a Dusklight checkout: missing $f" >&2; exit 1; }
done

# --- 1. Manifest -------------------------------------------------------------------------------

if grep -q "org.khronos.openxr.intent.category.IMMERSIVE_HMD" "$manifest"; then
    echo "manifest: already patched"
else
    python3 - "$manifest" <<'PY'
import sys

path = sys.argv[1]
text = open(path, encoding="utf-8").read()

# Headset features: head tracking is required, a touchscreen is not (already declared).
text = text.replace(
    '    <uses-feature android:glEsVersion="0x00020000" />',
    '    <uses-feature android:glEsVersion="0x00020000" />\n'
    '    <uses-feature android:name="android.hardware.vr.headtracking" android:required="true"\n'
    '        android:version="1" />',
    1,
)

# The OpenXR loader asks the runtime broker (a content provider) which runtime to load.
text = text.replace(
    "</manifest>",
    "    <queries>\n"
    '        <provider android:authorities="org.khronos.openxr.runtime_broker;'
    'org.khronos.openxr.system_runtime_broker" />\n'
    "    </queries>\n\n"
    "</manifest>",
    1,
)

# Store/device metadata: Quest device list and Pico's app type.
text = text.replace(
    "        <meta-data android:name=\"android.game_mode_config\"",
    '        <meta-data android:name="com.oculus.supportedDevices" android:value="quest2|questpro|quest3" />\n'
    '        <meta-data android:name="pvr.app.type" android:value="vr" />\n\n'
    "        <meta-data android:name=\"android.game_mode_config\"",
    1,
)

# Launching into VR: both the Khronos category and Meta's own are needed on the launcher filter.
text = text.replace(
    '                <category android:name="android.intent.category.LAUNCHER" />',
    '                <category android:name="android.intent.category.LAUNCHER" />\n'
    '                <category android:name="org.khronos.openxr.intent.category.IMMERSIVE_HMD" />\n'
    '                <category android:name="com.oculus.intent.category.VR" />',
    1,
)

open(path, "w", encoding="utf-8").write(text)
PY
    echo "manifest: patched"
fi

# --- 2. Dawn shared-texture features ------------------------------------------------------------

if grep -q "SharedTextureMemoryAHardwareBuffer" "$gpu"; then
    echo "aurora: already patched"
else
    python3 - "$gpu" <<'PY'
import sys

path = sys.argv[1]
text = open(path, encoding="utf-8").read()
anchor = "      const auto feature = supportedFeatures.features[i];"
if anchor not in text:
    raise SystemExit("aurora: could not find the feature loop in gpu.cpp")

text = text.replace(
    anchor,
    anchor + "\n"
    "      // Dusklight VR: let mods share rendered textures with other APIs (OpenXR).\n"
    "      switch (feature) {\n"
    "      case wgpu::FeatureName::SharedTextureMemoryAHardwareBuffer:\n"
    "      case wgpu::FeatureName::SharedTextureMemoryDmaBuf:\n"
    "      case wgpu::FeatureName::SharedTextureMemoryOpaqueFD:\n"
    "      case wgpu::FeatureName::SharedTextureMemoryVkDedicatedAllocation:\n"
    "      case wgpu::FeatureName::SharedFenceSyncFD:\n"
    "      case wgpu::FeatureName::SharedFenceVkSemaphoreOpaqueFD:\n"
    "        requiredFeatures.push_back(feature);\n"
    "        break;\n"
    "      default:\n"
    "        break;\n"
    "      }",
    1,
)
open(path, "w", encoding="utf-8").write(text)
PY
    echo "aurora: patched"
fi

echo "Dusklight VR edition patch applied."
