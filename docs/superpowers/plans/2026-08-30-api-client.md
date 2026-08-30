# API client 与 mock server 实现计划（阶段二 a）

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans. Steps use checkbox (`- [ ]`) syntax.

**Goal:** 一层可切换的 HTTP client、一个按文档实现的 mock server，以及把两者对起来的契约测试。服务端写完只改一个 baseURL。

**Architecture:** client 是纯 TS，不依赖任何 RN API（用全局 `fetch`，Node 22 和 RN 0.87 都有），所以契约测试在 jest 的 node 环境里跑，不需要模拟器。mock server 是零依赖的 `node:http` 脚本，既给测试当对手方，也给开发期的 App 用。

**Tech Stack:** TypeScript 6、RN 0.87.1、Jest 29、`node:http`。

**上游依据：** `docs/API-cloud-print.md` 第 4 节全部端点；`docs/superpowers/specs/2026-08-30-mobile-app-design.md` 第 5 节。

---

## 阶段一的教训，直接约束本阶段

URF 编码器那一轮，34 个测试在主机和 Android 真机上全绿，而格式是错的——它们只是
在自洽地验证一个错误的假设。**契约测试对着自己写的 mock 全绿，同样什么都不证明。**

所以：

1. mock server 的每条响应**逐字段抄文档**，不按 client 的方便来编。
2. 文档里给了 JSON 示例的端点，mock 就返回那个示例的形状。
3. 服务端一旦可用，第一件事是拿同一套契约测试打真服务端（`API_BASE` 环境变量切换），
   不是再写一套。这一条写进 `app/mock-server/README.md`。

---

## 设计决定

**client 不导出任何默认值常量。** spec 第 5.1 节。不能存在「profile 拿不到就用
默认尺寸」这条代码路径——所以 `RenderProfile` 的字段全是必填，没有 fallback。

**上传只构造请求，不发送。** 真正的上传在阶段三由原生后台任务做（200KB~15MB
不能过 JS 堆）。但请求头的构造逻辑两边必须是同一份，否则 `X-Printer-Serial`
或 RFC 5987 文件名会在某一侧写错。所以 client 导出 `buildPrintRequest()` 返回
`{url, method, headers}`，原生侧拿它去发；测试直接验这个对象。

**错误在 client 边界就转成 union。** UI 层不看状态码。

## 文件结构

| 文件 | 职责 |
|---|---|
| `app/src/api/types.ts` | 所有响应体的 TS 类型，逐个对着文档 |
| `app/src/api/errors.ts` | `ApiError` union 与 HTTP 状态码映射 |
| `app/src/api/rfc5987.ts` | `X-Filename` 的编码 |
| `app/src/api/http.ts` | fetch 封装：baseURL、`Authorization`、`X-Device` |
| `app/src/api/auth.ts` | 4.1b 的四个端点 |
| `app/src/api/devices.ts` | enroll / unbind / devices / render-profile / printer / printers |
| `app/src/api/jobs.ts` | 4.6 status |
| `app/src/api/print.ts` | `buildPrintRequest` |
| `app/src/api/tests.ts` | 4.8–4.10 适配测试 |
| `app/src/api/index.ts` | 对外出口 |
| `app/mock-server/server.mjs` | 零依赖 mock，`node:http` |
| `app/mock-server/state.mjs` | 内存状态：用户、设备、作业 |
| `app/src/api/__tests__/*.test.ts` | 契约测试，起 mock 打真 HTTP |

---

## Task 1: 工程骨架并入与脚本

**Files:** `app/package.json`、`app/.gitignore`、`app/jest.config.js`

- [ ] **Step 1: 合并 RN 脚手架**（已完成）：`npx @react-native-community/cli init AirPrint --version 0.87.1` 的产物 rsync 进 `app/`，保留已有的 `shared/` 与 `tools/`。

- [ ] **Step 2: package.json 加脚本**

```json
"scripts": {
  "android": "react-native run-android",
  "ios": "react-native run-ios",
  "lint": "eslint .",
  "start": "react-native start",
  "test": "jest",
  "mock": "node mock-server/server.mjs",
  "urf:test": "shared/urf/run_tests.sh",
  "urf:test:android": "shared/urf/run_tests_android.sh"
}
```

- [ ] **Step 3: .gitignore 加上 C++ 构建产物与 mock 的落盘目录**

```
shared/urf/build/
shared/urf/build-android/
mock-server/.data/
```

- [ ] **Step 4: 装依赖并确认 jest 能跑**

```bash
cd app && npm install && npx jest --listTests
```

期望：列出 `__tests__/App.test.tsx`。

- [ ] **Step 5: 提交**

---

## Task 2: 错误映射

先做这个，因为后面每个端点都要用它。

**Files:** `app/src/api/errors.ts`、`app/src/api/__tests__/errors.test.ts`

- [ ] **Step 1: 写失败的测试**（文档 4.1 的状态码表逐行一条用例，含 400 的 `detail` 要保留、401 不区分「设备不存在」和「密钥错」）
- [ ] **Step 2: 跑测试确认失败** `cd app && npx jest errors`
- [ ] **Step 3: 实现 `ApiError` union 与 `apiErrorFromResponse`**
- [ ] **Step 4: 跑测试确认通过**
- [ ] **Step 5: 提交**

## Task 3: RFC 5987 文件名编码

`X-Filename` 用它，中文文件名必须编对，否则显示乱码。

**Files:** `app/src/api/rfc5987.ts`、`app/src/api/__tests__/rfc5987.test.ts`

黄金用例直接取文档 4.4 的示例：`报告.pdf` → `UTF-8''%E6%8A%A5%E5%91%8A.pdf`。

- [ ] Step 1–5：同上的 TDD 循环。

## Task 4: HTTP 封装

**Files:** `app/src/api/http.ts`、`app/src/api/types.ts`

要点：

- `Authorization: Bearer <token>`，token 当不透明串，不解析不截断（文档 1.1）
- `X-Device` 只在需要时带（文档 4.1：App 侧才需要）
- 非 2xx 一律走 `apiErrorFromResponse`
- 网络异常转成 `{kind:'network'}`，不让 `TypeError` 漏到 UI

## Task 5: mock server

**Files:** `app/mock-server/server.mjs`、`app/mock-server/state.mjs`、`app/mock-server/README.md`

要点：

- 零依赖，`node:http`
- 端口 0（随机）时把实际端口写到 stdout，测试据此连接
- 验证码固定 `123456` 并打到 stdout（对应文档第 7 节「开发环境验证码只打日志」）
- 429 的四道闸只实现「同号码 60 秒间隔」一条，其余返回 200——**mock 的作用是
  验契约不是验限流**，实现四道闸只会让测试变脆
- `POST /api/print` 校验魔数与页数字段，不校验尺寸（尺寸校验要 render-profile
  联动，放在契约测试里显式构造）

## Task 6: 认证端点

**Files:** `app/src/api/auth.ts`、`app/src/api/__tests__/auth.contract.test.ts`

契约测试起 mock、打真 HTTP。覆盖：发码 → 验码拿 token → 用 token 调受保护端点 →
登出后同一 token 返回 401。

## Task 7: 设备端点

**Files:** `app/src/api/devices.ts`、`app/src/api/__tests__/devices.contract.test.ts`

覆盖 enroll（含 `reset:true` 与 409）、unbind 幂等、`GET /api/devices` 空列表返回
`{"devices":[]}` 而不是 404、render-profile 404 时的错误 union。

## Task 8: 作业与打印请求

**Files:** `app/src/api/jobs.ts`、`app/src/api/print.ts`、`app/src/api/__tests__/print.test.ts`

`buildPrintRequest` 的测试要断言：`Content-Type: image/urf`、`X-Printer-Serial`
必填（缺了就抛，不发请求）、`X-Filename` 是 RFC 5987、`X-Test-Run` 只在传了
`testRunId` 时出现。

## Task 9: 适配测试端点

**Files:** `app/src/api/tests.ts`、`app/src/api/__tests__/tests.contract.test.ts`

覆盖 4.8 清单、4.9 start（含 409 已有一轮在跑、`baseline_ok:false` 时带 `hint`）、
4.10 answer 的四种 verdict。

## 完成标准

- [ ] `cd app && npm test` 全绿
- [ ] `npm run mock` 起得来，文档第 7 节的 curl 步骤能对着它跑通
- [ ] `src/api` 里 grep 不到任何尺寸/dpi/色彩的默认值常量

## 不在本计划内

导航与登录界面、token 的 Keychain/Keystore 存储（阶段 2b），
原生光栅与上传（阶段三）。
