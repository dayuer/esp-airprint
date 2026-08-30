import React from 'react';
import {StyleSheet, Text, View} from 'react-native';
import {space, type} from '../theme';
import {useTheme} from '../useTheme';

/** 分组标题。小字用正字距——大字才要负的。 */
export function Section({title, children}: {title: string; children: React.ReactNode}) {
  const {c} = useTheme();
  return (
    <View style={styles.wrap}>
      <Text style={[type.label, styles.title, {color: c.inkFaint}]}>{title}</Text>
      <View style={[styles.rule, {backgroundColor: c.rule}]} />
      {children}
    </View>
  );
}

/**
 * 一行「名字 — 值」。值可以是拉丁标识（序列号、能力串），那种走 Literata。
 */
export function Row({
  label,
  value,
  ident,
  tone = 'normal',
}: {
  label: string;
  value: string;
  ident?: boolean;
  tone?: 'normal' | 'muted' | 'accent';
}) {
  const {c} = useTheme();
  const color = tone === 'accent' ? c.accent : tone === 'muted' ? c.inkMuted : c.ink;
  return (
    <View style={styles.row}>
      <Text style={[type.body, styles.rowLabel, {color: c.inkMuted}]}>{label}</Text>
      <Text
        style={[ident ? type.ident : type.body, styles.rowValue, {color}]}
        numberOfLines={2}>
        {value}
      </Text>
    </View>
  );
}

const styles = StyleSheet.create({
  wrap: {marginTop: space.xl},
  title: {paddingHorizontal: space.lg, marginBottom: space.sm},
  rule: {height: StyleSheet.hairlineWidth, marginHorizontal: space.lg},
  row: {
    flexDirection: 'row',
    alignItems: 'flex-start',
    paddingHorizontal: space.lg,
    paddingVertical: space.md,
  },
  rowLabel: {width: 96, marginRight: space.md},
  rowValue: {flex: 1, textAlign: 'right'},
});
