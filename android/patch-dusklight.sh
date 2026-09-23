#!/usr/bin/env bash
# Turns a Dusklight checkout into the "VR edition" the mod needs on Android.
#
# Additive changes (see the numbered sections below):
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
    '        android:version="1" />\n'
    # Quest blocks the launch with a "controllers required" dialog unless the app says it can run
    # without them.
    '    <uses-feature android:name="oculus.software.handtracking" android:required="false" />\n'
    # Tabletop see-through: without this feature Quest hides ALPHA_BLEND and XR_FB_passthrough.
    '    <uses-feature android:name="com.oculus.feature.PASSTHROUGH" android:required="false" />',
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


# --- 3. Separate app identity -------------------------------------------------------------------
# Own package name and label so the VR edition installs alongside the official app instead of
# replacing it (different signing key => Android would demand an uninstall, losing save data).

gradle="$root/platforms/android/app/build.gradle"
strings="$root/platforms/android/app/src/main/res/values/strings.xml"
if grep -q "dev.twilitrealm.dusk.vr" "$gradle"; then
    echo "identity: already patched"
else
    sed -i "s|applicationId: 'dev.twilitrealm.dusk',|applicationId: 'dev.twilitrealm.dusk.vr',|" "$gradle"
    sed -i 's|<string name="app_name">Dusklight</string>|<string name="app_name">Dusklight VR</string>|' "$strings"
    grep -q "dev.twilitrealm.dusk.vr" "$gradle" || { echo "identity: applicationId not found" >&2; exit 1; }
    echo "identity: patched (dev.twilitrealm.dusk.vr)"
fi

# --- 4. Export Aurora's symbols ------------------------------------------------------------------
# The Android link step filters exports through a generated version script, which drops Aurora's own
# functions. The mod hooks several of them by name (frame delivery, the RmlUi panel, the headset
# render size), so widen the export list. Costs a little symbol table, changes no behaviour.

exports="$root/cmake/AndroidExports.cmake"
if grep -q "_ZN6aurora" "$exports"; then
    echo "exports: already patched"
else
    sed -i 's|            --extra-sym "Java_\*"|            --extra-sym "Java_*"\n            # Dusklight VR: the mod hooks Aurora internals by name.\n            --extra-sym "_ZN6aurora*"\n            --extra-sym "_ZNK6aurora*"|' "$exports"
    grep -q "_ZN6aurora" "$exports" || { echo "exports: anchor not found" >&2; exit 1; }
    echo "exports: patched"
fi

# --- 5. Choose the disc from the headset ---------------------------------------------------------
# An immersive app never shows its 2D surface, and Dusklight's prelaunch screen (disc picker) draws
# there, so a first launch from the headset only shows the runtime's loading dots (the VR mod loads
# once a disc is set). When no disc is configured, open Android's document picker instead (the Quest
# shows it as a panel) before the game starts, keep read access to the chosen file, and pass it as
# --dvd: Dusklight validates it, saves it as the configured disc and boots into the game. Later
# launches use the saved disc. Cancelling the picker closes the app.

activity="$root/platforms/android/app/src/main/java/com/twilitrealm/dusk/DuskActivity.java"
if grep -q "VR_DISC_REQUEST_CODE" "$activity"; then
    echo "disc chooser: already patched"
else
    python3 - "$activity" <<'PY'
import sys

path = sys.argv[1]
text = open(path, encoding="utf-8").read()

old_return = "        return rawArgs == null ? arguments : splitArguments(rawArgs.trim());"
assert old_return in text, "getArguments return not found"
text = text.replace(
    old_return,
    "        if (rawArgs != null) {\n"
    "            return splitArguments(rawArgs.trim());\n"
    "        }\n"
    "        // Dusklight VR: the prelaunch screen can't be seen in the headset; pick the disc here.\n"
    "        if (hasConfiguredDisc()) {\n"
    "            return arguments;\n"
    "        }\n"
    "        String disc = chooseDiscImage();\n"
    "        if (disc == null) {\n"
    "            runOnUiThread(this::finishAndRemoveTask);\n"
    "            return arguments;\n"
    "        }\n"
    "        return new String[] {\"--dvd\", disc};",
    1,
)

anchor = "    @Override\n    protected String[] getArguments() {"
assert anchor in text, "getArguments not found"
text = text.replace(
    anchor,
    """    private static final int VR_DISC_REQUEST_CODE = 0x5652;
    private final java.util.concurrent.CountDownLatch vrDiscChosen = new java.util.concurrent.CountDownLatch(1);
    private volatile String vrDiscUri;

    // The disc Dusklight has saved (backend.isoPath in config.json), if it can still be read.
    private boolean hasConfiguredDisc() {
        File config = new File(getFilesDir(), "config.json");
        if (!config.isFile()) {
            return false;
        }
        try {
            String json = new String(java.nio.file.Files.readAllBytes(config.toPath()),
                java.nio.charset.StandardCharsets.UTF_8);
            String disc = new org.json.JSONObject(json).optString("backend.isoPath", "");
            if (disc.isEmpty()) {
                return false;
            }
            if (disc.startsWith("content://")) {
                try (android.os.ParcelFileDescriptor fd =
                         getContentResolver().openFileDescriptor(android.net.Uri.parse(disc), "r")) {
                    return fd != null;
                }
            }
            return new File(disc).canRead();
        } catch (Exception e) {
            Log.w(TAG, "Dusklight VR: configured disc unusable", e);
            return false;
        }
    }

    // Called on SDL's main thread (before the game starts): shows the system document picker and
    // waits for the choice. Returns a content:// URI, or null when cancelled.
    private String chooseDiscImage() {
        runOnUiThread(() -> {
            Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            intent.addCategory(Intent.CATEGORY_OPENABLE);
            intent.setType("*/*");
            intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
            try {
                startActivityForResult(intent, VR_DISC_REQUEST_CODE);
            } catch (android.content.ActivityNotFoundException e) {
                Log.w(TAG, "Dusklight VR: no document picker available", e);
                vrDiscChosen.countDown();
            }
        });
        try {
            vrDiscChosen.await();
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
        return vrDiscUri;
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        if (requestCode != VR_DISC_REQUEST_CODE) {
            super.onActivityResult(requestCode, resultCode, data);
            return;
        }
        if (resultCode == RESULT_OK && data != null && data.getData() != null) {
            android.net.Uri uri = data.getData();
            try {
                getContentResolver().takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
            } catch (SecurityException e) {
                Log.w(TAG, "Dusklight VR: could not keep access to " + uri, e);
            }
            Log.i(TAG, "Dusklight VR: disc chosen: " + uri);
            vrDiscUri = uri.toString();
        }
        vrDiscChosen.countDown();
    }

""" + anchor,
    1,
)
open(path, "w", encoding="utf-8").write(text)
PY
    echo "disc chooser: patched"
fi

echo "Dusklight VR edition patch applied."
