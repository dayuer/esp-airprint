import {ProfileSrc} from './types';

/**
 * 档案来源的可信度文案。逐条对应 docs/API-cloud-print.md 4.7 的表。
 *
 * 用户看不懂 `src: "quirks"`，但看得懂「通用配置，建议做一次测试」。
 * 这层翻译必须在一个地方，否则每个界面会各写一套说法。
 */
export function describeProfileSrc(src: ProfileSrc): string {
  switch (src) {
    case 'serial':
      return '已针对你的打印机校准';
    case 'model':
      return '已验证的机型配置';
    case 'authoritative':
      return '官方配置';
    case 'quirks':
      return '通用配置，建议做一次测试';
    case 'default':
      return '未适配，建议做一次测试';
  }
}

/** 该不该提示用户去做一次适配测试。 */
export function shouldSuggestTest(src: ProfileSrc, disputed: boolean): boolean {
  // disputed 表示同型号出现过相反结论，已回退到下一层——这时更该测。
  return disputed || src === 'quirks' || src === 'default';
}
