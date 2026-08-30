# URF 编码器

跨平台纯 C++，无平台依赖，供 iOS / Android 的 RasterKit TurboModule 共用。

## 跑测试

    ./run_tests.sh              # 开发机
    ./run_tests_android.sh      # 连着的 Android 真机（NDK 交叉编译 + adb）

不需要打印机。这是这个项目里唯一能在不烧纸的前提下验证光栅正确性的手段。

**改完编码器两个都要跑。** 真机那一趟不是走过场：第一次跑就抓到两个只在
Android 上暴露的问题——测试产物写死在 `/tmp`（Android 没有这个目录），
以及校验器把整个文件读进内存（A4 产物 35MB，峰值常驻内存 71MB）。
主机上两者都不会报错。

真机跑完会打印峰值常驻内存。当前值约 **5.4MB**（2MB 条带 + 64KB 读缓冲 +
运行时开销）。这个数字明显变大就说明有人在某处缓冲了整页。

## 用法

    urf::PageSpec spec;
    spec.width_px  = 4962;   // 来自 GET /api/device/{dev}/render-profile
    spec.height_px = 7014;
    spec.dpi       = 600;

    urf::Writer w("/tmp/job.urf");
    w.BeginPage(spec);
    while (还有条带) {
      渲染一带到 band（每行 spec.width_px 字节的 8 位灰度）;
      w.WriteRows(band, 本带行数);
    }
    w.EndPage();
    w.Close();                // 这一步才回填真实页数

    auto r = urf::ValidateForUpload("/tmp/job.urf", 4962, 7014);
    if (!r.ok) 中止上传，把 r.error 给用户;

峰值内存由条带高度决定，与页面尺寸无关。A4 600dpi 整页是 34.8MB，
不要构造整页缓冲。

## 三个容易写错的地方

1. **页数字段**。文件头第 8~11 字节，大端。写 0 打印机认为文档为空，什么都不打。
   `Writer` 先写占位 0，`Close()` 时 seek 回去回填——所以**不 Close 就上传等于废纸**。
2. **字面串长度不能是 1**。编码是 `257-len`，`len=1` 溢出成 256。单个不重复像素
   必须退回游程编码。见 `packbits.cpp` 里那个 `if (len == 1)`。
3. **行没有结束符**。行的结束是「像素数攒够 width」。曾经在每行末尾写 `0x80`，
   那会被解码成 129 个字面像素，整页从第一行起全烂——而任何环节都不会报错。
   守它的是 `必须认Apple光栅器的真实产物` 那条测试。
4. **校验器必须保持流式**。作业上限是 200MB（API 文档 4.4）。任何「先把文件读进
   `std::vector` 再扫」的改法都会在上传前自校验时把手机打死。

## 格式依据

字节布局来自 `tests/fixtures/apple-gray8-2480x3507-300dpi.urf`——Apple 自己的
光栅器产出的真实 URF。字段核对状态逐条记在 `HEADER-FIELDS.md`。

先前是从 `tools/reference/render.py` 的 `fix_page_count` 反推的，**推错了**：
那段扫描按 `0x80` 断行，而真实数据里 `0x80` 不作包首字节出现。
拿到真实样本之前，主机和 Android 上的 34 个测试全绿——它们只是在自洽地
验证一个错误的格式。**没有真实样本的格式测试不算验证。**
