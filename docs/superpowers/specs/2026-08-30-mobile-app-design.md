# 手机 App 设计（第一轮：账号 + 光栅打印）

面向 iOS / Android，React Native 0.87。目标是可上架的消费级产品。

对应 API：`docs/API-cloud-print.md`（v2 / `stickboxd`）。

---

## 1. 这个 App 为什么不是普通的设备管理端

API 文档第 6 节写死了一件事：**服务端不渲染，App 是唯一的光栅方**。

所以 App 里最重的一块不是 UI，是本地光栅流水线：

```
PDF / 图片 / 文本 / 网页 → 600dpi 8bit 灰度位图 → URF RLE → 上传
```

A4 单页 600dpi gray8 未压缩 **34MB**。这个量级碰不得 JS 堆，也不能整页驻留内存。
光栅管线必须是原生的，且必须逐条带处理。

第二件事：这条链路上**没有任何环节会发现错误**。服务端不解析文档，设备不认识
格式，打印机照单全收。尺寸错了、页数字段填 0、序列号对不上——症状都是一沓废纸。
所以本文第 6 节的每条约束都必须是代码里的断言，不是文档里的提醒。

## 2. 第一轮范围

**做**：账号（短信登录 / 登出 / 注销）、打印主链路（四种输入源 → 出纸）、
打印所需的最小设备面（设备列表、当前打印机、作业进度）。

**不做，留路由占位**：SoftAP 配网与 enroll、多打印机历史管理、
适配测试向导（API 4.8–4.10）、完整作业历史。

开发期用 API 文档第 7 节的 curl 手动 enroll 一台设备。

## 3. 目录结构

```
app/
├─ shared/urf/          纯 C++ URF 编码器 + PackBits，CMake/ctest
│   ├─ urf_encoder.{h,cpp}
│   ├─ packbits.{h,cpp}
│   └─ tests/
├─ ios/
│   ├─ StickBox/        主 app
│   ├─ StickBoxShare/   Share Extension target
│   └─ RasterKit/       ObjC++ TurboModule
├─ android/
│   ├─ app/             主 app（含 ACTION_SEND intent-filter）
│   └─ rasterkit/       JNI 桥 + 同一份 C++
└─ src/
    ├─ api/     HTTP client、类型、错误映射、mock server
    ├─ auth/    登录流程、token 存储
    ├─ print/   编排：选源 → 归一化 → 拉 profile → 光栅 → 上传 → 跟踪
    ├─ device/  设备列表、当前打印机、作业状态
    ├─ ui/      设计系统与共用组件
    └─ nav/     路由
```

`shared/urf` **无平台依赖**，能在开发机上编译和跑单测。编码器的正确性不该靠
插着打印机才验得出来。

## 4. 原生边界：JS 一个像素都不碰

两个 TurboModule，接口就是全部契约：

```ts
RasterKit.rasterize({
  sourceUri: string,
  sourceType: 'pdf' | 'image' | 'text' | 'html',
  dpi: number,              // 全部来自 render-profile
  colorModel: 'gray8',      // 不设默认值
  pageWidthPx: number,
  pageHeightPx: number,
  marginsMm: [number, number, number, number],
  outputPath: string,
}): Promise<{ pages: number; bytes: number; sha256: string }>
// 事件 onProgress: { page, totalPages, bytesWritten }
// 事件 onError: { code, message }

Uploader.upload({
  filePath: string, url: string, headers: Record<string, string>,
}): Promise<UploadResult>   // 后台任务，可取消
```

JS 拿到的只有文件路径和进度数字。像素和字节流从不进 JS 堆。

### 4.1 四种输入源收敛成一条管道

看着是四条路，实际先归一化成 PDF：

| 输入 | iOS | Android |
|---|---|---|
| PDF | 直接用 | 直接用 |
| 图片 | `UIGraphicsPDFRenderer` 画进页面 | `PdfDocument` 画进页面 |
| 文本 | `UIMarkupTextPrintFormatter` | `PrintDocumentAdapter` |
| 网页 | `WKWebView` → PDF | `WebView.createPrintDocumentAdapter()` |

之后只维护**一条** PDF → URF 管道，一个编码器，一套测试。

### 4.2 逐条带渲染

整页 34MB 不能驻留。渲染层按条带（band）输出，每带渲染完立刻喂给编码器、
立刻写盘、立刻释放。峰值内存由带高决定而不是页面尺寸。

带高取值使单带 ≤ 2MB；A4 600dpi 灰度每行 4962 字节，约 400 行一带。

### 4.3 图片源要做抖动

照片直接量化到 8bit 灰度会出现明显色阶。图片路径上加 Floyd–Steinberg 抖动。
PDF/文本/网页是矢量源，600dpi 下不需要。

## 5. API 层

### 5.1 客户端形态

`src/api` 导出按端点分组的函数，不导出任何默认值常量——**不能存在一条
「profile 拿不到就用默认尺寸」的代码路径**。

baseURL 可切换：第一轮指向 mock server，服务端就绪后改一个配置。

### 5.2 mock server

按 API 文档逐个端点实现，跑在本地。它同时是契约测试的对手方：
client 的每个函数都有一条对着 mock 的测试，文档改了先改 mock，测试立刻变红。

### 5.3 错误映射

HTTP 状态码在 client 边界就转成 discriminated union：

```ts
type ApiError =
  | { kind: 'unauthorized' }        // 401 → 清 token 回登录
  | { kind: 'forbidden' }           // 403
  | { kind: 'notFound' }            // 404
  | { kind: 'badRequest'; detail: string }   // 400，detail 直接展示
  | { kind: 'tooLarge' }            // 413
  | { kind: 'unsupportedMedia' }    // 415
  | { kind: 'rateLimited'; detail: string }  // 429
  | { kind: 'server'; detail: string }       // 500
  | { kind: 'network' }
```

UI 只对着这个 union 写文案，任何组件都不看状态码。

### 5.4 设备列表（新增端点）

API 文档 v2 里所有设备端点都要求调用方已经知道 `{dev}`，而 `dev` 唯一的来源是
配网时从 SoftAP 读到的 MAC。**缺一个「列出我名下的设备」。**

不补的话，设备列表只能存在手机本地，用户换手机或重装 App 后账号还在、设备还绑着，
但 App 再也找不到它。

契约已补进 `docs/API-cloud-print.md` 4.5b。App 按它实现，mock server 里先提供。

## 6. 正确性约束

文档第 6 节六条，全部落成代码里的断言：

| 约束 | 落点 | 错了会怎样 |
|---|---|---|
| 页头 9~12 字节大端页数非 0 | C++ 单测 + 上传前自校验 | 打印机认为文档为空，什么都不打 |
| 宽高等于 profile 里对应纸张 | 光栅前断言，不等不发请求 | 服务端 400；漏掉则错位或半页 |
| dpi / 色彩从 profile 读 | client 无默认值常量 | 换打印机后 App 要发版才跟得上 |
| render-profile 404 | 显式「打印机未就绪」，无 fallback | 蒙尺寸 = 废纸 |
| `X-Printer-Serial` 取自 profile | **上传前重查一次 serial** | 见下 |
| 429 不自动重试 | 倒计时 UI | 撞更严的限流闸 |

**上传前重查 serial 这条值得单说。** 光栅一份文档要几十秒，期间用户完全可能拔掉
打印机换一台。URF 是按某台机器的 dpi 和像素尺寸光栅的，派给另一台就是废纸，而
服务端不解析文档、设备不认识格式，没有任何环节会发现。所以上传前必须重新确认
序列号仍是光栅时那台，不一致就中止并提示用户。

## 7. 状态管理

| 状态 | 方案 | 理由 |
|---|---|---|
| 服务端状态（设备、作业、profile） | TanStack Query | 作业要轮询，缓存与失效逻辑现成 |
| 本地打印任务（光栅中 / 上传中） | Zustand | 跨屏幕存活，且要接原生事件 |
| session token | react-native-keychain | Keychain / Keystore |

token 无过期时间（API 文档 4.1b），下线靠吊销。401 是唯一的失效信号。

## 8. 测试策略

| 层 | 方式 |
|---|---|
| C++ 编码器 | CMake + ctest，在开发机上跑。黄金样本用文档第 7 节那份最小 URF |
| API client | 对着 mock server 的契约测试，每个端点一条 |
| 流程编排 | JS 单测，原生模块打桩 |
| 端到端 | mock server + 模拟器：选 PDF → 产出 .urf → 上传成功 |

编码器的单测是这个项目里最值钱的测试——它是唯一能在不插打印机、不烧纸的前提下
验证光栅正确性的手段。

## 9. 上架合规

消费级产品且目标上架，以下不是可选项：

- iOS `PrivacyInfo.xcprivacy`：声明相册、相机、文件访问用途
- 账号注销入口：API 4.1b 已有 `POST /api/account/delete`，必须在 App 内可达
- 隐私政策与用户协议：登录页可达，注册前明示
- 中国区：手机号登录涉及个人信息，需备案与隐私合规清单
- 权限文案：相册/相机的用途说明要具体，「用于打印你选择的照片」而不是「用于访问相册」

## 10. 风险

| 风险 | 应对 |
|---|---|
| 低端 Android 渲染大 PDF 被杀 | 逐条带 + 每页释放；在低内存设备上实测 |
| 移动网络下传 15MB 照片页 | 显示体积预估和进度，可取消；后台上传任务 |
| Share Extension 内存上限比主 app 严得多 | Extension 只负责接收和落盘，光栅交给主 app |
| RN 升级冲掉原生改动 | 原生改动集中在独立 target/module，不散在模板文件里 |

## 11. 明确不做

- 服务端渲染（已被否决，App 是唯一光栅方）
- 作业取消（服务端未提供）
- PCL 上传（服务端只放行 URF / PWG-Raster）
- 同时接多台打印机（协议不支持）
