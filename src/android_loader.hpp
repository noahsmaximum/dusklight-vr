#pragma once

// Android-only: the OpenXR loader needs the app's Java VM and a Context before it can find the
// runtime, and xrCreateInstance wants them chained in as well.
namespace vr::android {

bool initialize_loader();
void* java_vm();
void* application_context();
// The running Activity (falls back to the Application when it cannot be found).
void* activity();

} // namespace vr::android
