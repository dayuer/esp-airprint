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
    // 渲染在下一步。先把 JS → TurboModule → JNI → shared/urf 这条链打通，
    // 链没通的话渲染写完了也验不了。
    promise.reject("not_implemented", "渲染还没实现")
  }
}
