import type {TurboModule} from 'react-native';
import {TurboModuleRegistry} from 'react-native';

/**
 * 光栅模块。JS 侧只看到文件路径和进度数字——像素和字节流从不进 JS 堆。
 *
 * A4 600dpi 灰度整页是 34.8MB，这个量级碰不得 JS。渲染逐条带做，
 * 编码走 shared/urf 那份跨平台 C++（已在开发机和 Android 真机上验过）。
 */
export interface RasterizeOptions {
  /** 输入文件的 URI。第一轮只支持 PDF。 */
  sourceUri: string;
  /** 产物落在哪。调用方负责清理。 */
  outputPath: string;
  /** 全部来自 GET /api/device/{dev}/render-profile，不许有默认值。 */
  dpi: number;
  pageWidthPx: number;
  pageHeightPx: number;
}

export interface RasterizeResult {
  pages: number;
  bytes: number;
}

export interface ValidateResult {
  ok: boolean;
  pages: number;
  widthPx: number;
  heightPx: number;
  /** ok 为 false 时说明原因。 */
  error: string;
}

export interface Spec extends TurboModule {
  rasterize(options: RasterizeOptions): Promise<RasterizeResult>;

  /**
   * 上传前自校验。除了整份自洽，还要求首页尺寸等于 render-profile 给的值。
   *
   * 尺寸错了服务端会 400，漏过去就是错位或半页；而服务端不解析文档、
   * 设备不认识格式，没有任何环节会发现——所以本地必须自己拦。
   */
  validateForUpload(
    path: string,
    expectWidthPx: number,
    expectHeightPx: number,
  ): Promise<ValidateResult>;

  /** 编码器自检，用来确认原生链路通了。返回 shared/urf 的版本串。 */
  encoderVersion(): string;
}

/**
 * 用可空的 get 而不是 getEnforcing。
 *
 * getEnforcing 在**导入时**就抛，那意味着任何 import 到这条链的模块在没有原生
 * 模块的环境里（jest、将来的 web）都会直接崩，而且 try/catch 包在调用处拦不住。
 * 换成 get 之后，「原生模块没装上」变成一个可以处理的领域错误。
 */
export default TurboModuleRegistry.get<Spec>('RasterKit');
