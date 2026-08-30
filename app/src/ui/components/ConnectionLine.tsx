import React, {useEffect, useRef} from 'react';
import {Animated, StyleSheet, Text, View} from 'react-native';
import {BREATH_MS, space, type} from '../theme';
import {useTheme} from '../useTheme';
import {useReduceMotion} from '../useReduceMotion';

/**
 * 这个 App 的签名组件。
 *
 *     手机 ── 云 ── 桥 ── 打印机
 *
 * 断在哪一环，用户扫一眼就知道，不用读文字。
 *
 * 它对应 API 文档里的一个设计决定：服务端的 `offline` 既表示「桥断了」也表示
 * 「桥在但没插打印机」，服务端不区分（`seen` 新鲜就说明桥是活的）——但用户
 * 必须能区分，否则会以为设备坏了，而实际上只是打印机没插。
 */
export type LinkBreak = 'none' | 'bridge' | 'printer';

const NODES = ['手机', '云', '桥', '打印机'];
const DOT = 7;
const NODE_W = 44;

/** 断开的段索引：0=手机-云，1=云-桥，2=桥-打印机。none 时为 -1。 */
export function brokenSegment(broken: LinkBreak): number {
  return broken === 'bridge' ? 1 : broken === 'printer' ? 2 : -1;
}

/**
 * 这个节点要不要点亮成强色。
 *
 * `seg >= 0` 这个前置判断不能省：seg 为 -1 时 `i === seg + 1` 会命中 i=0，
 * 链路全通却把「手机」标成红的。类型和测试都不会报错，只有看屏幕才发现。
 */
export function isNodeLit(broken: LinkBreak, index: number): boolean {
  const seg = brokenSegment(broken);
  return seg >= 0 && (index === seg || index === seg + 1);
}

export function ConnectionLine({
  broken,
  showLabels = true,
}: {
  broken: LinkBreak;
  showLabels?: boolean;
}) {
  const {c} = useTheme();
  const reduceMotion = useReduceMotion();
  const breath = useRef(new Animated.Value(1)).current;
  const seg = brokenSegment(broken);

  useEffect(() => {
    if (seg < 0 || reduceMotion) {
      breath.setValue(1);
      return;
    }
    // 只有出问题时屏幕上才有东西在动。动本身就是告警。
    const loop = Animated.loop(
      Animated.sequence([
        Animated.timing(breath, {toValue: 0.3, duration: BREATH_MS / 2, useNativeDriver: true}),
        Animated.timing(breath, {toValue: 1, duration: BREATH_MS / 2, useNativeDriver: true}),
      ]),
    );
    loop.start();
    return () => loop.stop();
  }, [seg, reduceMotion, breath]);

  const label =
    broken === 'bridge' ? '打印桥离线'
    : broken === 'printer' ? '打印机没插上'
    : '链路正常';

  return (
    <View accessible accessibilityLabel={label} style={styles.row}>
      {NODES.map((name, i) => {
        const lit = isNodeLit(broken, i);
        return (
          <React.Fragment key={name}>
            <View style={[styles.node, showLabels ? {width: NODE_W} : null]}>
              <Animated.View
                style={[
                  styles.dot,
                  {backgroundColor: lit ? c.accent : c.inkMuted},
                  lit ? {opacity: breath} : null,
                ]}
              />
              {showLabels && (
                <Text
                  numberOfLines={1}
                  style={[type.label, styles.label, {color: lit ? c.accent : c.inkFaint}]}>
                  {name}
                </Text>
              )}
            </View>
            {i < NODES.length - 1 && (
              <View
                style={[
                  styles.seg,
                  // 断开的那一段不画线——留白比画一条红线更像「断了」
                  {backgroundColor: i === seg ? 'transparent' : c.rule},
                ]}
              />
            )}
          </React.Fragment>
        );
      })}
    </View>
  );
}

const styles = StyleSheet.create({
  row: {flexDirection: 'row', alignItems: 'flex-start'},
  node: {alignItems: 'center'},
  dot: {width: DOT, height: DOT, borderRadius: DOT / 2},
  label: {marginTop: space.xs},
  // 段要对齐圆点的垂直中心，所以往下让 (DOT-线宽)/2
  seg: {flex: 1, height: 1, marginTop: (DOT - 1) / 2, marginHorizontal: space.xs},
});
