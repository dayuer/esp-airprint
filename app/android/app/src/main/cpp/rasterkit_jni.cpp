#include <jni.h>
#include <string>
#include "urf/urf.h"

/*
 * JNI 桥。这里只做类型转换，不放任何逻辑——逻辑在 shared/urf 里，
 * 那份代码在开发机上有单测，放到这里就再也测不了了。
 */

namespace {

jstring to_jstring(JNIEnv* env, const std::string& s) {
  return env->NewStringUTF(s.c_str());
}

std::string from_jstring(JNIEnv* env, jstring s) {
  if (!s) return {};
  const char* p = env->GetStringUTFChars(s, nullptr);
  std::string out = p ? p : "";
  if (p) env->ReleaseStringUTFChars(s, p);
  return out;
}

}  // namespace

extern "C" {

JNIEXPORT jstring JNICALL
Java_com_airprint_rasterkit_UrfNative_encoderVersion(JNIEnv* env, jclass) {
  // 带上 colorspace，因为那是唯一一个填错不会报错、只会出废纸的常量。
  return to_jstring(env, "urf/1 bpp=8 colorspace=" +
                             std::to_string(urf::kColorspaceGray) +
                             " duplex=" + std::to_string(urf::kDuplexOneSided));
}

/**
 * 校验结果编码成一个字符串回给 Kotlin：ok|pages|w|h|error。
 * 为一个内部返回值定义一个 JNI 结构体不值当。
 */
JNIEXPORT jstring JNICALL
Java_com_airprint_rasterkit_UrfNative_validateForUpload(
    JNIEnv* env, jclass, jstring jpath, jint expectW, jint expectH) {
  urf::ValidateResult r = urf::ValidateForUpload(
      from_jstring(env, jpath), static_cast<uint32_t>(expectW),
      static_cast<uint32_t>(expectH));
  std::string out = std::string(r.ok ? "1" : "0") + "|" +
                    std::to_string(r.actual_pages) + "|" +
                    std::to_string(r.width_px) + "|" +
                    std::to_string(r.height_px) + "|" + r.error;
  return to_jstring(env, out);
}

}  // extern "C"
