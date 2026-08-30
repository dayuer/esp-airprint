import React, {useRef} from 'react';
import {Animated, Pressable as RNPressable, PressableProps, ViewStyle} from 'react-native';

type Props = Omit<PressableProps, 'children' | 'style'> & {
  children?: React.ReactNode;
  style?: ViewStyle | ViewStyle[];
};
import {PRESS_MS, PRESS_SCALE} from '../theme';

/**
 * 按压 scale(0.96)。用 timing 不用 spring：按压可以被中途取消，
 * timing 能立刻反向，spring 会先弹完。
 */
export function Pressable({children, style, ...rest}: Props) {
  const scale = useRef(new Animated.Value(1)).current;
  const to = (v: number) =>
    Animated.timing(scale, {toValue: v, duration: PRESS_MS, useNativeDriver: true}).start();

  return (
    <RNPressable
      onPressIn={() => to(PRESS_SCALE)}
      onPressOut={() => to(1)}
      {...rest}>
      <Animated.View style={[style, {transform: [{scale}]}]}>{children}</Animated.View>
    </RNPressable>
  );
}
