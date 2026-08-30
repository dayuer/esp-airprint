/**
 * 响应体类型，逐个对着 docs/API-cloud-print.md 第 4 节。
 *
 * 注意这里没有任何默认值——spec 第 5.1 节：不能存在「profile 拿不到就用默认
 * 尺寸」这条代码路径。所以 RenderProfile 的字段全是必填。
 */

/** `{key_id}.{secret}`，45 字符。当不透明串处理，不解析不截断不打日志。 */
export type Token = string;

/** ESP32 的 STA MAC 去掉冒号的小写十六进制，12 字符。 */
export type DeviceId = string;

export type DeviceState =
  | 'ready' | 'downloading' | 'printing' | 'done' | 'failed' | 'offline';

export type JobState = 'queued' | 'downloading' | 'printing' | 'done' | 'failed';

/** 打印机面板状态，服务端原样从设备心跳转发。 */
export interface PrinterPanel {
  code: number;
  display: string;
  online: boolean;
  asleep: boolean;
  paper_out: boolean;
  error: boolean;
}

// ---- 4.1b 认证 ----

export interface SmsResponse {
  ok: 1;
  ttl: number;      // 验证码有效秒数
}

export interface VerifyResponse {
  token: Token;
  user_id: string;
  phone_tail: string;
  new_user: boolean;
}

export interface AccountDeleteResponse {
  ok: 1;
  jobs_deleted: number;
}

export interface EnrollResponse {
  device_key: Token;
  dev: DeviceId;
  /** true 表示这台设备之前就绑在本账号上，旧密钥已被吊销。App 应提示「已重新绑定」。 */
  reset: boolean;
}

// ---- 4.5b 设备列表 ----

export interface DeviceListPrinter {
  serial: string;
  make: string;
  model: string;
  attached: boolean;
}

export interface DeviceListItem {
  dev: DeviceId;
  name: string;
  online: boolean;
  seen: number;
  state: DeviceState;
  bound: number;
  /** 没插打印机时为 null。 */
  printer: DeviceListPrinter | null;
  queued_jobs: number;
}

// ---- 4.5 光栅参数 ----

export interface PagePixels {
  w_px: number;
  h_px: number;
}

export interface RenderProfile {
  dev: DeviceId;
  /** 光栅前记下它，上传时用 X-Printer-Serial 带回来。 */
  serial: string;
  format: string;
  urf_caps: string;
  dpi: number;
  color: string;
  pages: Record<string, PagePixels>;
  /** 实测值，不是从能力串推的。 */
  margins_mm: [number, number, number, number];
  max_job_bytes: number;
  updated: number;
}

// ---- 4.6 状态 ----

export interface JobSummary {
  id: string;
  name: string;
  size: number;
  state: JobState;
  bytes: number;
  err: string;
  created: number;
  updated: number;
}

export interface StatusResponse {
  device: {
    dev: DeviceId;
    online: boolean;
    seen: number;
    state: DeviceState;
    prn: PrinterPanel | null;
  };
  /** 最多最近 15 件。 */
  jobs: JobSummary[];
}

// ---- 4.4 上传 ----

export interface PrintResponse {
  job: string;
  size: number;
  /** 服务端从 URF 头里读出来的页数，回给 App 核对。对不上说明编码器有问题。 */
  pages: number;
  state: JobState;
  /** false 表示目标打印机当前没插着。作业不失败也不删，留在队列里。 */
  printer_attached: boolean;
}

// ---- 4.7 / 4.7b 打印机 ----

/** 档案来源层级，决定 App 该怎么描述这份配置的可信度。 */
export type ProfileSrc = 'serial' | 'model' | 'authoritative' | 'quirks' | 'default';

export interface PrinterDetail {
  printer: {
    serial: string;
    vid: string;
    pid: string;
    make: string;
    model: string;
    cmd: string;
    urf_caps: string;
    proto: number;
    first_seen: number;
    last_seen: number;
  };
  profile: {
    uel_job_end: boolean;
    uel_wake: boolean;
    wake_delay_ms: number;
    iface_cycle: boolean;
    unidir: boolean;
    margins_mm: [number, number, number, number];
    src: ProfileSrc;
    votes: number;
    /** true 表示同型号出现过相反结论。App 应提示用户测一次。 */
    disputed: boolean;
  };
}

export interface PrinterListItem {
  serial: string;
  make: string;
  model: string;
  attached: boolean;
  profile_src: ProfileSrc;
  /** 为那台打印机排着、但它现在没插着的作业数。 */
  queued_jobs: number;
  first_seen: number;
  last_seen: number;
}

export interface PrinterList {
  /** 当前插着的那台的序列号，没插时为空串。 */
  attached: string;
  printers: PrinterListItem[];
}

// ---- 4.8 ~ 4.10 适配测试 ----

export type TestState = 'untested' | 'running' | 'passed' | 'failed' | 'unclear';
export type Verdict = 'pass' | 'fail' | 'unclear' | 'aborted';

export interface AdaptTest {
  id: string;
  name: string;
  required: boolean;
  jobs_needed: number;
  state: TestState;
  affects: string[];
}

export interface TestStartResponse {
  run_id: string;
  jobs_needed: number;
  variant: Record<string, unknown>;
  /** false 表示当前配置本身就打不出来，不要继续测变体。 */
  baseline_ok: boolean;
  instruction: string;
  hint?: string;
}

export interface TestAnswerResponse {
  applied: Record<string, unknown>;
  scope: string;
  votes: number;
  promoted: boolean;
  message: string;
}
