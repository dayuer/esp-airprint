import React from 'react';
import {ActivityIndicator, StyleSheet, Text, View} from 'react-native';
import {MIN_HIT, radius, space, type} from '../theme';
import {useTheme} from '../useTheme';
import {Pressable} from './Pressable';

/**
 * 主按钮是一块实心墨，像盖下去的章——圆角 2，不是胶囊。
 * 强色（accent）只留给销毁性操作，不做装饰。
 */
export function Button({
  title,
  onPress,
  disabled,
  loading,
  variant = 'primary',
}: {
  title: string;
  onPress: () => void;
  disabled?: boolean;
  loading?: boolean;
  variant?: 'primary' | 'quiet' | 'destructive';
}) {
  const {c} = useTheme();
  const off = disabled || loading;

  const bg =
    variant === 'primary' ? (off ? c.inkFaint : c.ink)
    : variant === 'destructive' ? (off ? c.inkFaint : c.accent)
    : 'transparent';
  const fg = variant === 'quiet' ? (off ? c.inkFaint : c.ink) : c.paper;

  return (
    <Pressable
      accessibilityRole="button"
      accessibilityState={{disabled: !!off, busy: !!loading}}
      disabled={off}
      onPress={onPress}
      style={[styles.base, {backgroundColor: bg}]}>
      <View style={styles.inner}>
        {loading && <ActivityIndicator size="small" color={fg} style={styles.spinner} />}
        <Text style={[type.body, styles.text, {color: fg}]}>{title}</Text>
      </View>
    </Pressable>
  );
}

const styles = StyleSheet.create({
  base: {
    minHeight: MIN_HIT + 4,
    borderRadius: radius.sm,
    justifyContent: 'center',
    paddingHorizontal: space.lg,
  },
  inner: {flexDirection: 'row', alignItems: 'center', justifyContent: 'center'},
  spinner: {marginRight: space.sm},
  text: {fontWeight: '600', textAlign: 'center'},
});
