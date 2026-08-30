# URF 编码器

跨平台纯 C++，无平台依赖，供 iOS / Android 的 RasterKit TurboModule 共用。

## 跑测试

    ./run_tests.sh

不需要手机，不需要打印机。这是这个项目里唯一能在不烧纸的前提下验证光栅正确性的手段。

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
3. **页头常量还没在真机核对过**。见 `HEADER-FIELDS.md`。第一次真机打印前必须核。

## 格式依据

字节布局全部来自 `tools/reference/render.py` 的 `fix_page_count`（真实跑通过的
扫描器）和 `docs/API-cloud-print.md` 第 7 节的最小样本。不是从网上抄的。
