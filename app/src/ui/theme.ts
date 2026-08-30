/**
 * 设计令牌。规范见 DESIGN.md。
 *
 * 十六进制是从 OKLCH 转过来的，改色时改 DESIGN.md 里的 OKLCH 再转换，
 * 不要直接调这里的十六进制——那样会丢掉感知均匀性。
 */
import {TextStyle} from 'react-native';

export interface Palette {
  paper: string;
  paperSunk: string;
  ink: string;
  inkMuted: string;
  inkFaint: string;
  rule: string;
  /** 只用于断开、故障、销毁性操作。屏幕上出现颜色 = 有一环出了问题。 */
  accent: string;
}

export const lightPalette: Palette = {
  paper: '#F7F4EE',
  paperSunk: '#EDE9E1',
  ink: '#252118',
  inkMuted: '#736E63',
  inkFaint: '#B4AFA4',
  rule: '#D9D4CA',
  accent: '#C0452C',
};

/** 反相纸，不是蓝灰色的通用 dark theme。 */
export const darkPalette: Palette = {
  paper: '#1C1A15',
  paperSunk: '#272419',
  ink: '#E8E3D8',        // 不是纯白：纯白在深底上太冲
  inkMuted: '#A19B8E',
  inkFaint: '#635E54',
  rule: '#3C382F',
  accent: '#DE6B4E',
};

/** 纸上没有圆角，所以只有三档且都很小。不用胶囊形。 */
export const radius = {none: 0, sm: 2, md: 6} as const;

export const space = {
  xs: 4, sm: 8, md: 16, lg: 24, xl: 32, xxl: 48,
} as const;

/**
 * Literata 只有拉丁字形。中文文本**不指定 fontFamily**（走系统的苹方 /
 * Noto Sans CJK），纯拉丁与数字的地方才显式用它。
 */
export const LATIN_FONT = 'Literata';

type Level = 'display' | 'title' | 'body' | 'label' | 'ident';

/** 大字要负字距。正字距在大标题上永远是错的；小字反而要正字距。 */
export const type: Record<Level, TextStyle> = {
  display: {fontSize: 34, lineHeight: 42, letterSpacing: -0.75, fontWeight: '600'},
  title:   {fontSize: 24, lineHeight: 32, letterSpacing: -0.29, fontWeight: '600'},
  body:    {fontSize: 16, lineHeight: 24, letterSpacing: 0},
  label:   {fontSize: 13, lineHeight: 18, letterSpacing: 0.2},
  /** 设备 ID、序列号：Literata + 宽字距，代替等宽字体。 */
  ident:   {fontSize: 15, lineHeight: 20, letterSpacing: 1.2, fontFamily: LATIN_FONT},
};

/** 断点呼吸的节奏。只有出问题时屏幕上才有东西在动。 */
export const BREATH_MS = 1600;

/** 按压反馈。可中断，所以用 timing 不用 spring。 */
export const PRESS_SCALE = 0.96;
export const PRESS_MS = 90;

/** 最小点击区。视觉元素更小时用透明扩展区补足。 */
export const MIN_HIT = 44;
