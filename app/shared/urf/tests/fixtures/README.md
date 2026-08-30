# 黄金样本

`apple-gray8-2480x3507-300dpi.urf` —— Apple 自己的光栅器产出的真实 URF，
用它反推的格式，也用它守住格式。

怎么来的（macOS，一次性取样，手机上永远不需要 CUPS）：

    printf 'sample\n' > /tmp/s.txt
    /usr/sbin/cupsfilter -m application/pdf /tmp/s.txt > /tmp/s.pdf
    /usr/sbin/cupsfilter -P /etc/cups/ppd/ESP32_BRIDGE.ppd -m image/urf \
        -o ColorModel=Gray -o PageSize=A4 /tmp/s.pdf > /tmp/real.urf

它确定了三件事，每件都推翻了先前的假设：

1. **行没有 `0x80` 结束符**。行的结束是「像素数攒够 width」。
   先前按 `tools/reference/render.py` 的 `fix_page_count` 推的模型是错的——
   那段扫描按 `0x80` 断行，在真实数据上会一路吞到文件尾。
2. **行重复计数是必需的，不是可选优化**。这份 2480x3507 的页只有 100 条记录，
   10470 字节；每行独立编码要 147KB。
3. **`duplex` 字节是 1 不是 0**。URF 的 duplex 枚举里 1 = 单面，0 不是合法值。
