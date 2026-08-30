package com.airprint.rasterkit

/** shared/urf 那份 C++ 的薄封装。只做 JNI 声明，不放逻辑。 */
object UrfNative {
  init {
    System.loadLibrary("rasterkit")
  }

  @JvmStatic external fun encoderVersion(): String

  /** 返回 `ok|pages|w|h|error`。 */
  @JvmStatic external fun validateForUpload(
    path: String,
    expectWidthPx: Int,
    expectHeightPx: Int,
  ): String
}
