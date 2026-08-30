package com.airprint.rasterkit

/**
 * shared/urf 那份 C++ 的薄封装。只做 JNI 声明，不放逻辑。
 *
 * 返回约定：空串表示成功，非空表示错误信息。
 */
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

  /** 返回句柄，0 表示打不开输出文件。 */
  @JvmStatic external fun jobOpen(outputPath: String): Long

  @JvmStatic external fun jobBeginPage(handle: Long, widthPx: Int, heightPx: Int, dpi: Int): String

  /** 直接锁 ARGB_8888 位图的像素做灰度化，不额外拷贝一份。 */
  @JvmStatic external fun jobWriteBandBitmap(handle: Long, bitmap: android.graphics.Bitmap, rows: Int): String

  @JvmStatic external fun jobEndPage(handle: Long): String

  /** 成功返回 `pages|bytes`，失败返回以 `!` 开头的错误。句柄一律释放。 */
  @JvmStatic external fun jobFinish(handle: Long): String

  @JvmStatic external fun jobAbort(handle: Long)
}
