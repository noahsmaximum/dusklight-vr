#include "android_loader.hpp"

#include "mods/svc/log.hpp"

#include <jni.h>

#define XR_USE_PLATFORM_ANDROID
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <dlfcn.h>

// On Android the OpenXR loader has to be initialised with the app's Java VM and a Context before
// anything else, and the instance carries them too. Dusklight's release build exports no SDL
// symbols, so both come from the Java runtime directly: JNI_GetCreatedJavaVMs for the VM, and
// ActivityThread.currentApplication() for the Context.
namespace vr::android {
namespace {

JavaVM* g_vm = nullptr;
jobject g_context = nullptr; // global ref
bool g_ready = false;

JavaVM* find_java_vm() {
    using GetCreatedJavaVMs = jint (*)(JavaVM**, jsize, jsize*);
    GetCreatedJavaVMs getVms = nullptr;
    for (const char* lib : {"libnativehelper.so", "libart.so", "libdvm.so"}) {
        if (void* handle = dlopen(lib, RTLD_NOW | RTLD_LOCAL)) {
            getVms = reinterpret_cast<GetCreatedJavaVMs>(dlsym(handle, "JNI_GetCreatedJavaVMs"));
            if (getVms != nullptr) {
                break;
            }
        }
    }
    if (getVms == nullptr) {
        mods::log::error("Android: could not find JNI_GetCreatedJavaVMs");
        return nullptr;
    }
    JavaVM* vms[1] = {};
    jsize count = 0;
    if (getVms(vms, 1, &count) != JNI_OK || count < 1) {
        mods::log::error("Android: no Java VM in this process");
        return nullptr;
    }
    return vms[0];
}

// ActivityThread.currentApplication() -> Application (a Context), without needing the Activity.
jobject find_application_context(JNIEnv* env) {
    jclass activityThread = env->FindClass("android/app/ActivityThread");
    if (activityThread == nullptr) {
        return nullptr;
    }
    jmethodID currentApplication =
        env->GetStaticMethodID(activityThread, "currentApplication", "()Landroid/app/Application;");
    if (currentApplication == nullptr) {
        return nullptr;
    }
    jobject application = env->CallStaticObjectMethod(activityThread, currentApplication);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return nullptr;
    }
    return application;
}

} // namespace

bool initialize_loader() {
    if (g_ready) {
        return true;
    }
    g_vm = find_java_vm();
    if (g_vm == nullptr) {
        return false;
    }
    JNIEnv* env = nullptr;
    if (g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK &&
        g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK)
    {
        mods::log::error("Android: could not attach to the Java VM");
        return false;
    }
    jobject context = find_application_context(env);
    if (context == nullptr) {
        mods::log::error("Android: could not reach the application context");
        return false;
    }
    g_context = env->NewGlobalRef(context);

    PFN_xrInitializeLoaderKHR initializeLoader = nullptr;
    if (XR_FAILED(xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
            reinterpret_cast<PFN_xrVoidFunction*>(&initializeLoader))) ||
        initializeLoader == nullptr)
    {
        mods::log::error("Android: the OpenXR loader has no xrInitializeLoaderKHR");
        return false;
    }
    XrLoaderInitInfoAndroidKHR info{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
    info.applicationVM = g_vm;
    info.applicationContext = g_context;
    if (XR_FAILED(initializeLoader(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&info)))) {
        mods::log::error("Android: xrInitializeLoaderKHR failed (is an OpenXR runtime installed?)");
        return false;
    }
    g_ready = true;
    mods::log::info("Android: OpenXR loader initialised");
    return true;
}

void* java_vm() { return g_vm; }
void* application_context() { return g_context; }

} // namespace vr::android
