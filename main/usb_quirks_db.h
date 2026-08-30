/*
 * CUPS USB quirks 表 —— 自动生成，请勿手改。
 *
 * 来源：OpenPrinting CUPS, backend/org.cups.usb-quirks
 *       https://github.com/OpenPrinting/cups  (Apache License 2.0)
 * 生成方式见 docs/HANDOFF-cloud-print.md「兼容性数据来源」一节。
 *
 * 只导入在本平台上说得通的 quirk：
 *   no-reattach —— Linux usblp 内核模块专用，ESP32 上没有这个概念，已丢弃
 *   whitelist   —— 只是「确认可用」的注记，不产生行为，已丢弃
 *
 * 这张表是【冷启动的起点】，不是完整的兼容性库：全表只有几十个可用条目，
 * 且高度偏向佳能和惠普的老喷墨。真正吃苦头的那些怪癖（作业结束符要不要发、
 * 唤醒等多久、URF 能不能断流）CUPS 里【没有】——因为在 Linux 上它们不发生。
 */
#pragma once
#include <stdint.h>

#define QK_BLACKLIST     0x01   /* CUPS 判定该机型不能用 USB 后端 */
#define QK_UNIDIR        0x02   /* 只支持单向 I/O：不要读 bulk IN、不要发 PJL */
#define QK_SOFT_RESET    0x04   /* 打印后需要 SOFT_RESET 收尾 */
#define QK_DELAY_CLOSE   0x08   /* 释放接口前要留延时 */
#define QK_USB_INIT      0x10   /* 需要厂商私有初始化串（本项目未实现） */
#define QK_VENDOR_CLASS  0x20   /* 接口是厂商私有 class/subclass，不是 7/1/x */
#define QK_NO_ALT_SET    0x40   /* 不要发 SET_INTERFACE */

typedef struct { uint16_t vid, pid; uint8_t flags; } usb_quirk_t;

#define QUIRK_PID_ANY 0xFFFF    /* 该厂商所有产品 */

static const usb_quirk_t CUPS_USB_QUIRKS[] = {
    { 0x03F0, 0x0004,       QK_UNIDIR                                  },  /* unidir */
    { 0x03F0, 0x0104,       QK_UNIDIR                                  },  /* unidir */
    { 0x03F0, 0x0204,       QK_UNIDIR                                  },  /* unidir */
    { 0x03F0, 0x0304,       QK_UNIDIR                                  },  /* unidir */
    { 0x03F0, 0x0404,       QK_UNIDIR                                  },  /* unidir */
    { 0x03F0, 0x0504,       QK_UNIDIR                                  },  /* unidir */
    { 0x03F0, 0x0604,       QK_UNIDIR                                  },  /* unidir */
    { 0x03F0, 0x0804,       QK_UNIDIR                                  },  /* unidir */
    { 0x03F0, 0x0C17,       QK_DELAY_CLOSE                             },  /* delay-close */
    { 0x03F0, 0x0E17,       QK_DELAY_CLOSE                             },  /* delay-close */
    { 0x03F0, 0x0F17,       QK_DELAY_CLOSE                             },  /* delay-close */
    { 0x03F0, 0x1017,       QK_DELAY_CLOSE                             },  /* delay-close */
    { 0x03F0, 0x1104,       QK_UNIDIR                                  },  /* unidir */
    { 0x03F0, 0x1117,       QK_DELAY_CLOSE                             },  /* delay-close */
    { 0x03F0, 0x1D17,       QK_DELAY_CLOSE                             },  /* delay-close */
    { 0x03F0, 0x1E17,       QK_DELAY_CLOSE                             },  /* delay-close */
    { 0x0409, 0xBEF4,       QK_UNIDIR                                  },  /* unidir */
    { 0x0409, 0xEFBE,       QK_UNIDIR                                  },  /* unidir */
    { 0x0409, 0xF0BE,       QK_UNIDIR                                  },  /* unidir */
    { 0x0409, 0xF1BE,       QK_UNIDIR                                  },  /* unidir */
    { 0x0482, 0x0010,       QK_UNIDIR                                  },  /* unidir */
    { 0x0482, 0x033E,       QK_SOFT_RESET                              },  /* soft-reset */
    { 0x04A9, 0x1095,       QK_UNIDIR                                  },  /* unidir */
    { 0x04A9, 0x10A2,       QK_UNIDIR                                  },  /* unidir */
    { 0x04A9, 0x10B6,       QK_UNIDIR                                  },  /* unidir */
    { 0x04A9, 0x1702,       QK_UNIDIR                                  },  /* unidir */
    { 0x04A9, 0x1703,       QK_UNIDIR                                  },  /* unidir */
    { 0x04A9, 0x170B,       QK_UNIDIR                                  },  /* unidir */
    { 0x04A9, 0x170C,       QK_UNIDIR                                  },  /* unidir */
    { 0x04A9, 0x1712,       QK_UNIDIR                                  },  /* unidir */
    { 0x04A9, 0x1712,       QK_UNIDIR                                  },  /* unidir */
    { 0x04A9, 0x1716,       QK_UNIDIR                                  },  /* unidir */
    { 0x04A9, 0x1717,       QK_UNIDIR                                  },  /* unidir */
    { 0x04A9, 0x1721,       QK_UNIDIR                                  },  /* unidir */
    { 0x04A9, 0x1722,       QK_UNIDIR                                  },  /* unidir */
    { 0x04A9, 0x1723,       QK_UNIDIR                                  },  /* unidir */
    { 0x04A9, 0x1724,       QK_UNIDIR                                  },  /* unidir */
    { 0x04A9, 0x1728,       QK_UNIDIR                                  },  /* unidir */
    { 0x04A9, 0x1730,       QK_UNIDIR                                  },  /* unidir */
    { 0x04A9, 0x1731,       QK_UNIDIR                                  },  /* unidir */
    { 0x04A9, 0x1732,       QK_UNIDIR                                  },  /* unidir */
    { 0x04A9, 0x1733,       QK_UNIDIR                                  },  /* unidir */
    { 0x04A9, 0x1736,       QK_UNIDIR                                  },  /* unidir */
    { 0x04A9, 0x173A,       QK_UNIDIR                                  },  /* unidir */
    { 0x04A9, 0x173B,       QK_UNIDIR                                  },  /* unidir */
    { 0x04A9, 0x173C,       QK_UNIDIR                                  },  /* unidir */
    { 0x04A9, 0x173D,       QK_UNIDIR                                  },  /* unidir */
    { 0x04A9, 0x173E,       QK_UNIDIR                                  },  /* unidir */
    { 0x04A9, 0x1746,       QK_UNIDIR                                  },  /* unidir */
    { 0x04A9, 0x1747,       QK_UNIDIR                                  },  /* unidir */
    { 0x04A9, 0x1757,       QK_UNIDIR                                  },  /* unidir */
    { 0x04A9, 0x175F,       QK_UNIDIR                                  },  /* unidir */
    { 0x04A9, 0x1796,       QK_UNIDIR                                  },  /* unidir */
    { 0x04A9, 0x304A,       QK_BLACKLIST                               },  /* blacklist */
    { 0x04A9, 0x3063,       QK_BLACKLIST                               },  /* blacklist */
    { 0x04A9, 0x307C,       QK_BLACKLIST                               },  /* blacklist */
    { 0x04A9, 0x307D,       QK_BLACKLIST                               },  /* blacklist */
    { 0x04A9, 0x30BD,       QK_BLACKLIST                               },  /* blacklist */
    { 0x04A9, 0x30BE,       QK_BLACKLIST                               },  /* blacklist */
    { 0x04A9, 0x30F5,       QK_BLACKLIST                               },  /* blacklist */
    { 0x04A9, 0x30F6,       QK_BLACKLIST                               },  /* blacklist */
    { 0x04A9, 0x310B,       QK_BLACKLIST                               },  /* blacklist */
    { 0x04A9, 0x3127,       QK_BLACKLIST                               },  /* blacklist */
    { 0x04A9, 0x3128,       QK_BLACKLIST                               },  /* blacklist */
    { 0x04A9, 0x3141,       QK_BLACKLIST                               },  /* blacklist */
    { 0x04A9, 0x3142,       QK_BLACKLIST                               },  /* blacklist */
    { 0x04A9, 0x3143,       QK_BLACKLIST                               },  /* blacklist */
    { 0x04A9, 0x3170,       QK_BLACKLIST                               },  /* blacklist */
    { 0x04A9, 0x3171,       QK_BLACKLIST                               },  /* blacklist */
    { 0x04A9, 0x3185,       QK_BLACKLIST                               },  /* blacklist */
    { 0x04A9, 0x3186,       QK_BLACKLIST                               },  /* blacklist */
    { 0x04A9, 0x31AA,       QK_BLACKLIST                               },  /* blacklist */
    { 0x04A9, 0x31AB,       QK_BLACKLIST                               },  /* blacklist */
    { 0x04A9, 0x31AF,       QK_BLACKLIST                               },  /* blacklist */
    { 0x04A9, 0x31B0,       QK_BLACKLIST                               },  /* blacklist */
    { 0x04A9, 0x31DD,       QK_BLACKLIST                               },  /* blacklist */
    { 0x04A9, 0x31DD,       QK_BLACKLIST                               },  /* blacklist */
    { 0x04A9, 0x31EE,       QK_BLACKLIST                               },  /* blacklist */
    { 0x04A9, 0x3214,       QK_BLACKLIST                               },  /* blacklist */
    { 0x04A9, 0x3255,       QK_BLACKLIST                               },  /* blacklist */
    { 0x04A9, 0x3256,       QK_BLACKLIST                               },  /* blacklist */
    { 0x04B8, 0x0001,       QK_UNIDIR                                  },  /* no-reattach unidir */
    { 0x04B8, 0x0202,       QK_VENDOR_CLASS                            },  /* vendor-class */
    { 0x04E8, 0x330F,       QK_UNIDIR                                  },  /* unidir */
    { 0x04E8, 0x3460,       QK_NO_ALT_SET                              },  /* whitelist no-alt-set */
    { 0x04E8, QUIRK_PID_ANY, QK_SOFT_RESET                              },  /* soft-reset */
    { 0x04F9, 0x000D,       QK_UNIDIR                                  },  /* no-reattach unidir */
    { 0x04F9, 0x000E,       QK_UNIDIR                                  },  /* no-reattach unidir */
    { 0x0519, 0x0001,       QK_DELAY_CLOSE                             },  /* delay-close */
    { 0x0519, QUIRK_PID_ANY, QK_UNIDIR                                  },  /* unidir */
    { 0x067B, 0x2305,       QK_SOFT_RESET | QK_UNIDIR                  },  /* no-reattach soft-reset unidir */
    { 0x06BC, 0x0183,       QK_UNIDIR                                  },  /* unidir */
    { 0x0A5F, QUIRK_PID_ANY, QK_UNIDIR                                  },  /* unidir no-reattach */
    { 0x195F, 0x0001,       QK_UNIDIR                                  },  /* unidir no-reattach */
    { 0x2730, 0x2008,       QK_UNIDIR | QK_DELAY_CLOSE                 },  /* unidir delay-close */
    { 0x2D84, QUIRK_PID_ANY, QK_UNIDIR                                  },  /* unidir no-reattach */
};

#define CUPS_USB_QUIRKS_N (sizeof CUPS_USB_QUIRKS / sizeof CUPS_USB_QUIRKS[0])
