#include <rex/filesystem.h>

#include <SDL3/SDL_system.h>
#include <jni.h>

#include <string>

namespace rex::filesystem {

bool IsAndroidContentUri(const std::string_view source) {
  return source.starts_with("content://");
}

int OpenAndroidContentFileDescriptor(const std::string_view uri, const char* mode) {
  auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
  if (!env || !IsAndroidContentUri(uri) || !mode) {
    return -1;
  }
  // SDL's Java helper detaches the ParcelFileDescriptor. The caller owns the
  // returned descriptor, including closing it if mmap fails.
  jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());
  if (!activity) {
    return -1;
  }
  jclass type = env->GetObjectClass(activity);
  jmethodID open = type ? env->GetStaticMethodID(
                             type, "openFileDescriptor",
                             "(Ljava/lang/String;Ljava/lang/String;)I")
                       : nullptr;
  jstring java_uri = open ? env->NewStringUTF(std::string(uri).c_str()) : nullptr;
  jstring java_mode = java_uri ? env->NewStringUTF(mode) : nullptr;
  int descriptor = -1;
  if (java_mode) {
    descriptor = env->CallStaticIntMethod(type, open, java_uri, java_mode);
  }
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    descriptor = -1;
  }
  if (java_mode) env->DeleteLocalRef(java_mode);
  if (java_uri) env->DeleteLocalRef(java_uri);
  if (type) env->DeleteLocalRef(type);
  env->DeleteLocalRef(activity);
  return descriptor;
}

}  // namespace rex::filesystem
