package com.airprint.rasterkit

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File

/**
 * 光栅链路的设备端回归测试：PdfRenderer → JNI → shared/urf。
 *
 * 这条链只有在设备上才成立——开发机上的 jest 和 ctest 都覆盖不到 PdfRenderer。
 *
 *     ./gradlew connectedDebugAndroidTest
 */
@RunWith(AndroidJUnit4::class)
class PdfRasterizerTest {

  private val ctx = InstrumentationRegistry.getInstrumentation().targetContext

  /** A4 600dpi，取自 render-profile 的示例值。 */
  private val a4W = 4962
  private val a4H = 7014

  private fun copyAsset(name: String): File {
    val out = File(ctx.cacheDir, name)
    InstrumentationRegistry.getInstrumentation().context.assets.open(name).use { input ->
      out.outputStream().use { input.copyTo(it) }
    }
    return out
  }

  /** /proc/self/status 的 VmHWM，单位 KB。 */
  private fun peakRssKb(): Long =
    File("/proc/self/status").readLines()
      .firstOrNull { it.startsWith("VmHWM:") }
      ?.filter { it.isDigit() }?.toLongOrNull() ?: -1

  @Test
  fun 两页PDF光栅成两页URF且尺寸正确() {
    val pdf = copyAsset("two-page.pdf")
    // 落到外部目录，好让开发机 adb pull 回去用独立编译的校验器复核——
    // 设备端自检和开发机复核用的是同一份 C++，但不是同一次编译。
    val outDir = ctx.getExternalFilesDir(null) ?: ctx.cacheDir
    val urf = File(outDir, "out.urf")

    val r = PdfRasterizer.rasterize(
      ctx, pdf.absolutePath, urf.absolutePath, dpi = 600,
      pageWidthPx = a4W, pageHeightPx = a4H,
    )

    assertEquals("页数要和 PDF 一致", 2, r.pages)
    assertTrue("产物不该是空的", r.bytes > 0)

    // 用 shared/urf 那份已经在开发机上跑过 35 个单测的校验器验产物。
    // 它同时检查了魔数、页数字段非 0、逐页行数完整、以及尺寸等于 render-profile。
    val out = UrfNative.validateForUpload(urf.absolutePath, a4W, a4H)
    val parts = out.split("|")
    assertEquals("校验失败：${parts.getOrNull(4)}", "1", parts[0])
    assertEquals("2", parts[1])
    assertEquals(a4W.toString(), parts[2])
    assertEquals(a4H.toString(), parts[3])
  }

  @Test
  fun 尺寸与renderprofile不符时校验要拒绝() {
    val pdf = copyAsset("two-page.pdf")
    val urf = File(ctx.cacheDir, "out2.urf")
    PdfRasterizer.rasterize(ctx, pdf.absolutePath, urf.absolutePath, 600, 1240, 1754)

    // 按 A5 光栅却声称是 A4，服务端会 400；漏过去就是错位或半页。
    val out = UrfNative.validateForUpload(urf.absolutePath, a4W, a4H).split("|")
    assertEquals("0", out[0])
    assertTrue("错误信息要给出期望值", out[4].contains("4962"))
  }

  /**
   * 峰值内存与页面尺寸无关——这是整个设计的要害。
   *
   * 不用绝对阈值：进程里有 PDFium 的页缓存和字体缓存，一次 A4 光栅的增量是
   * 30MB 上下，而整页灰度正好也是 34.8MB，绝对阈值根本分辨不出「分带」和
   * 「缓冲整页」。
   *
   * 改成量「页面变高之后多花了多少」：先跑一遍 A4 让那些一次性缓存就位，
   * 再跑一个 4 倍高的页面。分带的话增量接近 0；谁改成整页缓冲，4 倍高就是
   * 多 139MB（灰度）或 557MB（ARGB），一眼就能看出来。
   */
  @Test
  fun 页面变高不该让峰值内存跟着涨() {
    val pdf = copyAsset("two-page.pdf")

    // 预热：让 PDFium 的页缓存、字体缓存、位图池都就位。
    PdfRasterizer.rasterize(
      ctx, pdf.absolutePath, File(ctx.cacheDir, "warm.urf").absolutePath,
      600, a4W, a4H,
    )

    val before = peakRssKb()
    assertTrue("读不到 VmHWM", before > 0)

    // 4 倍高。整页灰度会是 139MB，整页 ARGB 是 557MB。
    PdfRasterizer.rasterize(
      ctx, pdf.absolutePath, File(ctx.cacheDir, "tall.urf").absolutePath,
      600, a4W, a4H * 4,
    )
    val after = peakRssKb()
    val delta = after - before

    assertTrue(
      "页高翻 4 倍后峰值涨了 ${delta}KB（${before} -> ${after}）。" +
        "分带的话这个数应该接近 0，涨这么多说明某处缓冲了整页。",
      delta < 8 * 1024,
    )
  }

  /**
   * 内容真的落到纸上了。
   *
   * 前面那些测试有个漏洞：变换矩阵写错、整页渲染成全白的话，页数、尺寸、
   * 校验全都照样通过——而那正是「一沓白纸」的症状。
   *
   * 用同尺寸的空白 PDF 做基线：全白页靠行重复压到极小，有字的页会明显更大。
   * 比的是两者的比值，不是绝对字节数，所以换 PDF、换 dpi 都不用改这条。
   */
  @Test
  fun 渲染出来的不能是一张白纸() {
    val blankUrf = File(ctx.cacheDir, "blank.urf")
    val textUrf = File(ctx.cacheDir, "text.urf")

    val blank = PdfRasterizer.rasterize(
      ctx, copyAsset("blank.pdf").absolutePath, blankUrf.absolutePath, 600, a4W, a4H,
    )
    val text = PdfRasterizer.rasterize(
      ctx, copyAsset("two-page.pdf").absolutePath, textUrf.absolutePath, 600, a4W, a4H,
    )

    // 两页对一页，先按页数归一。
    val blankPerPage = blank.bytes / blank.pages
    val textPerPage = text.bytes / text.pages

    assertTrue(
      "有字的页每页 ${textPerPage} 字节，空白页每页 ${blankPerPage} 字节——" +
        "差不多大就说明什么都没画上去，出的是白纸。",
      textPerPage > blankPerPage * 3,
    )
  }

  @Test
  fun 条带高度由内存预算定与页高无关() {
    // 这是整个设计的要害：页面变高不会让一条带变大。
    val a4 = PdfRasterizer.bandRowsFor(a4W, a4H)
    val tall = PdfRasterizer.bandRowsFor(a4W, a4H * 10)
    assertEquals(a4, tall)
    assertTrue("一条带的 ARGB 不该超过 4MB", a4W * a4 * 4 <= 4 * 1024 * 1024)
    // 页面很矮时不该超过页高。
    assertEquals(3, PdfRasterizer.bandRowsFor(a4W, 3))
  }
}
