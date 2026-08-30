#include <android/bitmap.h>
#include <jni.h>

#include <memory>
#include <string>
#include <vector>

#include "urf/urf.h"

/*
 * JNI 桥。这里只做类型转换、灰度化和错误传递，编码逻辑在 shared/urf 里——
 * 那份代码在开发机上有单测，挪到这里就再也测不了了。
 *
 * 返回约定：空串表示成功，非空表示错误信息。Kotlin 侧非空即抛。
 */

namespace {

/** 一次光栅作业。Writer 跨多次 JNI 调用存活，用 jlong 句柄带在 Kotlin 那边。 */
struct Job {
  urf::Writer writer;
  /** 灰度条带的暂存。跨条带复用，避免每带都分配几 MB。 */
  std::vector<uint8_t> gray;

  explicit Job(const std::string& path) : writer(path) {}
};

Job* as_job(jlong h) { return reinterpret_cast<Job*>(h); }

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

/** 校验结果编码成 `ok|pages|w|h|error`。为一个内部返回值定义 JNI 结构体不值当。 */
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

/** 打开输出文件。返回 0 表示失败，错误信息取不到——调用方按「打不开文件」处理。 */
JNIEXPORT jlong JNICALL
Java_com_airprint_rasterkit_UrfNative_jobOpen(JNIEnv* env, jclass, jstring jpath) {
  try {
    return reinterpret_cast<jlong>(new Job(from_jstring(env, jpath)));
  } catch (...) {
    return 0;
  }
}

JNIEXPORT jstring JNICALL
Java_com_airprint_rasterkit_UrfNative_jobBeginPage(
    JNIEnv* env, jclass, jlong h, jint w, jint hgt, jint dpi) {
  Job* j = as_job(h);
  if (!j) return to_jstring(env, "作业句柄无效");
  try {
    urf::PageSpec s;
    s.width_px = static_cast<uint32_t>(w);
    s.height_px = static_cast<uint32_t>(hgt);
    s.dpi = static_cast<uint32_t>(dpi);
    j->writer.BeginPage(s);
    j->gray.resize(static_cast<size_t>(w));   // 至少一行，写带时按需扩
    return to_jstring(env, "");
  } catch (const std::exception& e) {
    return to_jstring(env, e.what());
  }
}

/**
 * 写一条带，像素直接从 Bitmap 里锁出来。
 *
 * 不走 direct ByteBuffer + copyPixelsToBuffer：那要多留一份和位图一样大的缓冲，
 * 而且每条带白拷贝一次几 MB。AndroidBitmap_lockPixels 给的是位图自己的内存。
 */
JNIEXPORT jstring JNICALL
Java_com_airprint_rasterkit_UrfNative_jobWriteBandBitmap(
    JNIEnv* env, jclass, jlong h, jobject bitmap, jint rows) {
  Job* j = as_job(h);
  if (!j) return to_jstring(env, "作业句柄无效");

  AndroidBitmapInfo info;
  if (AndroidBitmap_getInfo(env, bitmap, &info) != ANDROID_BITMAP_RESULT_SUCCESS)
    return to_jstring(env, "读不到位图信息");
  if (info.format != ANDROID_BITMAP_FORMAT_RGBA_8888)
    return to_jstring(env, "位图不是 ARGB_8888");
  if (static_cast<uint32_t>(rows) > info.height)
    return to_jstring(env, "条带行数超过位图高度");

  void* pixels = nullptr;
  if (AndroidBitmap_lockPixels(env, bitmap, &pixels) != ANDROID_BITMAP_RESULT_SUCCESS)
    return to_jstring(env, "锁不住位图像素");

  const auto width = static_cast<size_t>(info.width);
  const size_t n = width * static_cast<size_t>(rows);
  if (j->gray.size() < n) j->gray.resize(n);

  // stride 未必等于 width*4，逐行按 stride 走。
  const auto* base = static_cast<const uint8_t*>(pixels);
  for (jint y = 0; y < rows; ++y) {
    const uint8_t* p = base + static_cast<size_t>(y) * info.stride;
    uint8_t* g = j->gray.data() + static_cast<size_t>(y) * width;
    for (size_t x = 0; x < width; ++x, p += 4) {
      // Rec.601 亮度。整数版本：(77R + 150G + 29B + 128) >> 8。
      g[x] = static_cast<uint8_t>((77u * p[0] + 150u * p[1] + 29u * p[2] + 128u) >> 8);
    }
  }
  AndroidBitmap_unlockPixels(env, bitmap);

  try {
    j->writer.WriteRows(j->gray.data(), static_cast<uint32_t>(rows));
    return to_jstring(env, "");
  } catch (const std::exception& e) {
    return to_jstring(env, e.what());
  }
}

JNIEXPORT jstring JNICALL
Java_com_airprint_rasterkit_UrfNative_jobEndPage(JNIEnv* env, jclass, jlong h) {
  Job* j = as_job(h);
  if (!j) return to_jstring(env, "作业句柄无效");
  try {
    j->writer.EndPage();
    return to_jstring(env, "");
  } catch (const std::exception& e) {
    return to_jstring(env, e.what());
  }
}

/** 成功返回 `pages|bytes`，失败返回以 `!` 开头的错误信息。句柄一律释放。 */
JNIEXPORT jstring JNICALL
Java_com_airprint_rasterkit_UrfNative_jobFinish(JNIEnv* env, jclass, jlong h) {
  std::unique_ptr<Job> j(as_job(h));
  if (!j) return to_jstring(env, "!作业句柄无效");
  try {
    // Close 里才回填真实页数——不 Close 就上传等于废纸。
    j->writer.Close();
    return to_jstring(env, std::to_string(j->writer.pages()) + "|" +
                               std::to_string(j->writer.bytes_written()));
  } catch (const std::exception& e) {
    return to_jstring(env, std::string("!") + e.what());
  }
}

/** 取消或出错时释放句柄。产物文件由 Kotlin 侧删。 */
JNIEXPORT void JNICALL
Java_com_airprint_rasterkit_UrfNative_jobAbort(JNIEnv*, jclass, jlong h) {
  delete as_job(h);
}

}  // extern "C"
