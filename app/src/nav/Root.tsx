import React, {useEffect, useState} from 'react';
import {ActivityIndicator, StatusBar, View} from 'react-native';
import {SafeAreaProvider} from 'react-native-safe-area-context';
import {SessionProvider, useSession} from '../auth/context';
import {createSessionStore} from '../auth/session';
import {keychainTokenStore} from '../auth/tokenStore';
import {CodeScreen} from '../screens/CodeScreen';
import {DeviceListScreen} from '../screens/DeviceListScreen';
import {PhoneScreen} from '../screens/PhoneScreen';
import {useTheme} from '../ui/useTheme';
import {API_BASE_URL} from '../config';

const store = createSessionStore({baseUrl: API_BASE_URL, store: keychainTokenStore});

function Flow() {
  const {c, dark} = useTheme();
  const session = useSession();
  const phase = session(s => s.phase);
  const [phone, setPhone] = useState<string | null>(null);

  useEffect(() => {
    session.getState().bootstrap();
  }, [session]);

  // 登出后要把登录流程退回第一步，否则再次登录会直接落在验证码页
  useEffect(() => {
    if (phase === 'signedOut') setPhone(null);
  }, [phase]);

  return (
    <View style={{flex: 1, backgroundColor: c.paper}}>
      {/* RN 0.87 起 Android 强制 edge-to-edge，StatusBar 不再接受 backgroundColor，
          底色由页面自己铺。 */}
      <StatusBar barStyle={dark ? 'light-content' : 'dark-content'} />
      {phase === 'booting' ? (
        <View style={{flex: 1, justifyContent: 'center'}}>
          <ActivityIndicator color={c.inkMuted} />
        </View>
      ) : phase === 'signedIn' ? (
        <DeviceListScreen />
      ) : phone === null ? (
        <PhoneScreen onSent={setPhone} />
      ) : (
        <CodeScreen phone={phone} onBack={() => setPhone(null)} />
      )}
    </View>
  );
}

export function Root() {
  return (
    <SafeAreaProvider>
      <SessionProvider store={store}>
        <Flow />
      </SessionProvider>
    </SafeAreaProvider>
  );
}
