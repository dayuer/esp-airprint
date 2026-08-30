import React from 'react';
import {StyleSheet, TextInput, TextInputProps, View} from 'react-native';
import {LATIN_FONT, MIN_HIT, radius, space, type} from '../theme';
import {useTheme} from '../useTheme';

/**
 * 下沉区用 paperSunk，与 paper 差约 3% 亮度——在纸的语言里这已经够了，
 * 它模拟的是压痕不是投影。所以没有边框也没有阴影。
 */
export function Field({latin, ...props}: TextInputProps & {latin?: boolean}) {
  const {c} = useTheme();
  return (
    <View style={[styles.wrap, {backgroundColor: c.paperSunk}]}>
      <TextInput
        placeholderTextColor={c.inkFaint}
        selectionColor={c.ink}
        style={[
          type.body,
          styles.input,
          {color: c.ink},
          latin ? {fontFamily: LATIN_FONT, letterSpacing: 1.2} : null,
        ]}
        {...props}
      />
    </View>
  );
}

const styles = StyleSheet.create({
  wrap: {borderRadius: radius.sm, minHeight: MIN_HIT + 6, justifyContent: 'center'},
  input: {paddingHorizontal: space.md, paddingVertical: space.sm},
});
