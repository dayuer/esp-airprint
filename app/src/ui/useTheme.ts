import {useColorScheme} from 'react-native';
import {Palette, darkPalette, lightPalette} from './theme';

export function useTheme(): {c: Palette; dark: boolean} {
  const dark = useColorScheme() === 'dark';
  return {c: dark ? darkPalette : lightPalette, dark};
}
