import React, {useEffect, useState} from 'react';
import {ActivityIndicator, StatusBar, View} from 'react-native';
import {NavigationContainer, DefaultTheme, Theme} from '@react-navigation/native';
import {createNativeStackNavigator} from '@react-navigation/native-stack';
import {SafeAreaProvider} from 'react-native-safe-area-context';
import {DeviceListItem} from '../api';
import {SessionProvider, useSession} from '../auth/context';
import {createSessionStore} from '../auth/session';
import {keychainTokenStore} from '../auth/tokenStore';
import {CodeScreen} from '../screens/CodeScreen';
import {DeviceDetailScreen} from '../screens/DeviceDetailScreen';
import {DeviceListScreen} from '../screens/DeviceListScreen';
import {PhoneScreen} from '../screens/PhoneScreen';
import {AdaptTestScreen} from '../screens/AdaptTestScreen';
import {ProvisionScreen} from '../screens/ProvisionScreen';
import {useTheme} from '../ui/useTheme';
import {API_BASE_URL} from '../config';

const store = createSessionStore({baseUrl: API_BASE_URL, store: keychainTokenStore});

export type AppStackParams = {
  DeviceList: undefined;
  DeviceDetail: {device: DeviceListItem};
  Provision: undefined;
  AdaptTest: {dev: string};
};

const Stack = createNativeStackNavigator<AppStackParams>();

function AppStack() {
  const {c} = useTheme();
  return (
    <Stack.Navigator
      screenOptions={{
        headerStyle: {backgroundColor: c.paper},
        headerTintColor: c.ink,
        // 标题不指定 fontFamily：Literata 没有中文字形，设备名可能是中文。
        headerTitleStyle: {fontWeight: '600'},
        headerShadowVisible: false,   // 纸上没有投影，分隔靠发丝线
        contentStyle: {backgroundColor: c.paper},
      }}>
      <Stack.Screen name="DeviceList" options={{headerShown: false}}>
        {({navigation}) => (
          <DeviceListScreen
            onOpen={device => navigation.navigate('DeviceDetail', {device})}
            onAdd={() => navigation.navigate('Provision')}
          />
        )}
      </Stack.Screen>
      <Stack.Screen name="Provision" options={{title: '', headerBackTitle: '设备'}}>
        {({navigation}) => <ProvisionScreen onDone={() => navigation.goBack()} />}
      </Stack.Screen>
      <Stack.Screen name="AdaptTest" options={{title: '', headerBackTitle: '返回'}}>
        {({route}) => <AdaptTestScreen dev={route.params.dev} />}
      </Stack.Screen>
      <Stack.Screen name="DeviceDetail" options={{title: '', headerBackTitle: '设备'}}>
        {({route, navigation}) => (
          <DeviceDetailScreen
            device={route.params.device}
            onUnbound={() => navigation.goBack()}
            onRunTests={dev => navigation.navigate('AdaptTest', {dev})}
          />
        )}
      </Stack.Screen>
    </Stack.Navigator>
  );
}

function AuthFlow() {
  const [phone, setPhone] = useState<string | null>(null);
  return phone === null ? (
    <PhoneScreen onSent={setPhone} />
  ) : (
    <CodeScreen phone={phone} onBack={() => setPhone(null)} />
  );
}

function Flow() {
  const {c, dark} = useTheme();
  const session = useSession();
  const phase = session(s => s.phase);

  useEffect(() => {
    session.getState().bootstrap();
  }, [session]);

  const navTheme: Theme = {
    ...DefaultTheme,
    dark,
    colors: {
      ...DefaultTheme.colors,
      background: c.paper,
      card: c.paper,
      text: c.ink,
      border: c.rule,
      primary: c.ink,
      notification: c.accent,
    },
    fonts: DefaultTheme.fonts,
  };

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
        <NavigationContainer theme={navTheme}>
          <AppStack />
        </NavigationContainer>
      ) : (
        // 每次回到未登录都重挂一次，登录流程自然退回第一步而不是停在验证码页。
        <AuthFlow key="auth" />
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
