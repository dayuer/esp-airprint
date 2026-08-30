package com.airprint.rasterkit

import android.content.Context
import android.graphics.Bitmap
import android.graphics.Color
import android.graphics.Matrix
import android.graphics.pdf.PdfRenderer
import android.net.Uri
import android.os.ParcelFileDescriptor
import java.io.File
import kotlin.math.max
import kotlin.math.min

/**
 * PDF → 8 位灰度 URF。
 *
 * **峰值内存与页面尺寸无关。** A4 600dpi 灰度整页是 4962×7014 = 34.8MB，
 * 整页缓冲在低端机上会被直接杀掉。这里逐条带渲染，位图和缓冲跨条带、跨页复用。
 */
object PdfRasterizer {

  /** 一条带的 ARGB 位图占多少字节。8 位灰度只有它的四分之一，瓶颈在这。 */
  private const val BAND_BUDGET_BYTES = 4 * 1024 * 1024

  class RasterizeException(message: String) : Exception(message)

  data class Result(val pages: Int, val bytes: Long)

  fun rasterize(
    context: Context,
    sourceUri: String,
    outputPath: String,
    dpi: Int,
    pageWidthPx: Int,
    pageHeightPx: Int,
  ): Result {
    require(pageWidthPx > 0 && pageHeightPx > 0) { "页面尺寸必须为正" }

    openDescriptor(context, sourceUri).use { fd ->
      PdfRenderer(fd).use { renderer ->
        if (renderer.pageCount == 0) throw RasterizeException("这份 PDF 一页都没有")

        val handle = UrfNative.jobOpen(outputPath)
        if (handle == 0L) throw RasterizeException("打不开输出文件：$outputPath")

        try {
          val bandRows = bandRowsFor(pageWidthPx, pageHeightPx)
          // 跨条带、跨页复用。每页重新分配的话，20 页就是 20 次几 MB 的分配。
          val bitmap = Bitmap.createBitmap(pageWidthPx, bandRows, Bitmap.Config.ARGB_8888)

          try {
            for (i in 0 until renderer.pageCount) {
              renderPage(renderer, i, handle, bitmap, bandRows, pageWidthPx, pageHeightPx, dpi)
            }
          } finally {
            bitmap.recycle()
          }

          val out = UrfNative.jobFinish(handle)
          if (out.startsWith("!")) throw RasterizeException(out.substring(1))
          val parts = out.split("|")
          return Result(parts[0].toInt(), parts[1].toLong())
        } catch (e: Throwable) {
          UrfNative.jobAbort(handle)
          File(outputPath).delete()   // 半份产物比没有产物更危险
          throw e
        }
      }
    }
  }

  private fun renderPage(
    renderer: PdfRenderer,
    index: Int,
    handle: Long,
    bitmap: Bitmap,
    bandRows: Int,
    pageWidthPx: Int,
    pageHeightPx: Int,
    dpi: Int,
  ) {
    renderer.openPage(index).use { page ->
      check(UrfNative.jobBeginPage(handle, pageWidthPx, pageHeightPx, dpi))

      // PDF 的单位是 point（1/72 英寸）。等比缩放塞进目标尺寸并居中，
      // 多出来的地方留白——拉伸会把文档变形，那比留白难解释得多。
      val scale = min(
        pageWidthPx.toFloat() / page.width,
        pageHeightPx.toFloat() / page.height,
      )
      val offsetX = (pageWidthPx - page.width * scale) / 2f
      val offsetY = (pageHeightPx - page.height * scale) / 2f

      var top = 0
      while (top < pageHeightPx) {
        val rows = min(bandRows, pageHeightPx - top)

        // PdfRenderer 不画背景。不擦成白的话，没被 PDF 覆盖的像素是透明，
        // 转灰度后是黑的——整页会变成一张黑纸。
        bitmap.eraseColor(Color.WHITE)

        val m = Matrix()
        m.setScale(scale, scale)
        m.postTranslate(offsetX, offsetY - top)
        page.render(bitmap, null, m, PdfRenderer.Page.RENDER_MODE_FOR_PRINT)

        check(UrfNative.jobWriteBandBitmap(handle, bitmap, rows))

        top += rows
      }

      check(UrfNative.jobEndPage(handle))
    }
  }

  /** 条带高度由内存预算定，与页面高度无关——这是整个设计的要害。 */
  internal fun bandRowsFor(widthPx: Int, heightPx: Int): Int {
    val rows = BAND_BUDGET_BYTES / max(1, widthPx * 4)
    return max(1, min(rows, heightPx))
  }

  private fun check(err: String) {
    if (err.isNotEmpty()) throw RasterizeException(err)
  }

  private fun openDescriptor(context: Context, uri: String): ParcelFileDescriptor {
    // 文档选择器给的是 content://，命令行和测试给的是路径或 file://。
    return when {
      uri.startsWith("content://") ->
        context.contentResolver.openFileDescriptor(Uri.parse(uri), "r")
          ?: throw RasterizeException("打不开：$uri")
      else -> {
        val path = if (uri.startsWith("file://")) Uri.parse(uri).path ?: uri else uri
        val f = File(path)
        if (!f.exists()) throw RasterizeException("文件不存在：$path")
        ParcelFileDescriptor.open(f, ParcelFileDescriptor.MODE_READ_ONLY)
      }
    }
  }
}
