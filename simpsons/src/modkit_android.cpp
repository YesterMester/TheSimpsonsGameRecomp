#include "runtime.h"

#include <jni.h>
#include <algorithm>

namespace {
using namespace simpsons::modkit;
fs::path Path(JNIEnv* env, jstring text) {
  const auto size = env->GetStringLength(text);
  const auto* chars = env->GetStringChars(text, nullptr);
  if (!chars) throw Failure("invalid_path", "Cannot read the selected path");
  const std::u16string path(reinterpret_cast<const char16_t*>(chars), size);
  env->ReleaseStringChars(text, chars);
  return fs::path(path);
}
jstring Text(JNIEnv* env, const Json& value) {
  const auto text = value.dump();
  auto bytes = env->NewByteArray(static_cast<jsize>(text.size()));
  if (!bytes) return nullptr;
  env->SetByteArrayRegion(bytes, 0, static_cast<jsize>(text.size()), reinterpret_cast<const jbyte*>(text.data()));
  auto type = env->FindClass("java/lang/String");
  auto constructor = env->GetMethodID(type, "<init>", "([BLjava/lang/String;)V");
  return static_cast<jstring>(env->NewObject(type, constructor, bytes, env->NewStringUTF("UTF-8")));
}
Json Describe(const Manifest& m) {
  auto result = m.source;
  result["id"] = m.id;
  result["name"] = m.name;
  result["version"] = m.version;
  result["author"] = m.author;
  result["kind"] = m.kind;
  result["description"] = m.description;
  result["folder"] = Utf8(m.folder);
  result["fingerprint"] = m.fingerprint;
  return result;
}
}  // namespace

extern "C" JNIEXPORT jstring JNICALL
Java_io_github_yestermester_simpsons_ModPackages_validateNative(
    JNIEnv* env, jclass, jstring folder, jboolean contents) {
  try { return Text(env, {{"ok", true}, {"mod", Describe(Manifest::Read(Path(env, folder), contents))}}); }
  catch (const std::exception& error) { return Text(env, {{"ok", false}, {"msg", error.what()}}); }
}

extern "C" JNIEXPORT jstring JNICALL
Java_io_github_yestermester_simpsons_ModPackages_resolveNative(
    JNIEnv* env, jclass, jstring root, jstring requested, jboolean contents) {
  try {
    const auto directory = Path(env, root);
    const char* text = env->GetStringUTFChars(requested, nullptr);
    if (!text) throw Failure("invalid_profile", "Cannot read the selected mods");
    const std::string selection(text);
    env->ReleaseStringUTFChars(requested, text);
    const auto enabled = Json::parse(selection).get<std::vector<std::string>>();
    std::vector<Manifest> all;
    if (fs::is_directory(directory))
      for (const auto& entry : fs::directory_iterator(directory))
        if (entry.is_directory() && !Utf8(entry.path().filename()).starts_with('.') &&
            fs::is_regular_file(entry.path() / "mod.json"))
          all.push_back(Manifest::Read(entry.path(), false));
    auto resolved = Resolve(std::move(all), enabled);
    Json result = Json::array();
    for (auto& manifest : resolved) {
      if (contents) {
        manifest = Manifest::Read(manifest.folder);
        if (manifest.kind == "native" && !manifest.binaries.contains("android-arm64"))
          throw Failure("unsupported_platform", manifest.name + " needs an Android ARM64 binary");
      }
      result.push_back(Describe(manifest));
    }
    return Text(env, {{"ok", true}, {"mods", std::move(result)}});
  } catch (const std::exception& error) { return Text(env, {{"ok", false}, {"msg", error.what()}}); }
}
