import NativeRasterKit, {
  RasterizeOptions, RasterizeResult, Spec, ValidateResult,
} from './NativeRasterKit';

export class RasterKitUnavailable extends Error {
  constructor() {
    super('原生光栅模块未加载');
    this.name = 'RasterKitUnavailable';
  }
}

function mod(): Spec {
  if (!NativeRasterKit) throw new RasterKitUnavailable();
  return NativeRasterKit;
}

/** 原生模块在不在。JS-only 环境（jest）里是 false。 */
export function isRasterKitAvailable(): boolean {
  return NativeRasterKit != null;
}

/**
 * 光栅模块的 JS 门面。
 *
 * 这一层不做任何默认值——尺寸和 dpi 全部来自 render-profile。
 * 「profile 拿不到就用默认尺寸」这条路径在代码里不存在。
 */
export const RasterKit = {
  /** shared/urf 的版本与关键常量。用来确认原生链路通了。 */
  version(): string {
    return mod().encoderVersion();
  },

  rasterize(options: RasterizeOptions): Promise<RasterizeResult> {
    return mod().rasterize(options);
  },

  validateForUpload(
    path: string,
    expectWidthPx: number,
    expectHeightPx: number,
  ): Promise<ValidateResult> {
    return mod().validateForUpload(path, expectWidthPx, expectHeightPx);
  },
};

export type {RasterizeOptions, RasterizeResult, ValidateResult};
