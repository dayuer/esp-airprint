package com.airprint.rasterkit

import com.airprint.specs.NativeRasterKitSpec
import com.facebook.react.bridge.Arguments
import com.facebook.react.bridge.Promise
import com.facebook.react.bridge.ReactApplicationContext
import com.facebook.react.bridge.ReadableMap
import com.facebook.react.module.annotations.ReactModule

@ReactModule(name = NativeRasterKitSpec.NAME)
class RasterKitModule(reactContext: ReactApplicationContext) :
  NativeRasterKitSpec(reactContext) {

  override fun getName(): String = NAME

  override fun encoderVersion(): String = UrfNative.encoderVersion()

  override fun validateForUpload(
    path: String,
    expectWidthPx: Double,
    expectHeightPx: Double,
    promise: Promise,
  ) {
    try {
      // C++ 侧把结果编码成 ok|pages|w|h|error。
      val parts = UrfNative.validateForUpload(
        path, expectWidthPx.toInt(), expectHeightPx.toInt(),
      ).split("|", limit = 5)
      val map = Arguments.createMap().apply {
        putBoolean("ok", parts[0] == "1")
        putDouble("pages", parts[1].toDouble())
        putDouble("widthPx", parts[2].toDouble())
        putDouble("heightPx", parts[3].toDouble())
        putString("error", parts.getOrElse(4) { "" })
      }
      promise.resolve(map)
    } catch (e: Throwable) {
      promise.reject("validate_failed", e.message, e)
    }
  }

  override fun rasterize(options: ReadableMap, promise: Promise) {
    // 光栅是重活：A4 整页要几秒。放主线程会卡住整个界面。
    Thread {
      try {
        val r = PdfRasterizer.rasterize(
          context = reactApplicationContext,
          sourceUri = options.getString("sourceUri") ?: "",
          outputPath = options.getString("outputPath") ?: "",
          dpi = options.getDouble("dpi").toInt(),
          pageWidthPx = options.getDouble("pageWidthPx").toInt(),
          pageHeightPx = options.getDouble("pageHeightPx").toInt(),
        )
        promise.resolve(
          Arguments.createMap().apply {
            putDouble("pages", r.pages.toDouble())
            putDouble("bytes", r.bytes.toDouble())
          },
        )
      } catch (e: Throwable) {
        promise.reject("rasterize_failed", e.message, e)
      }
    }.start()
  }
}
