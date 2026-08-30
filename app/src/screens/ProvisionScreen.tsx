import React, {useCallback, useEffect, useState} from 'react';
import {
  ActivityIndicator, KeyboardAvoidingView, Platform, ScrollView, StyleSheet, Text, View,
} from 'react-native';
import {useSafeAreaInsets} from 'react-native-safe-area-context';
import {ApiFailure, describeApiError} from '../api';
import {Button} from '../ui/components/Button';
import {Field} from '../ui/components/Field';
import {Pressable} from '../ui/components/Pressable';
import {Row, Section} from '../ui/components/Section';
import {LATIN_FONT, space, type} from '../ui/theme';
import {useTheme} from '../ui/useTheme';
import {useSession} from '../auth/context';
import {PORTAL_BASE_URL} from '../config';
import {PortalFailure, PortalNetwork} from '../provisioning/portal';
import {
  isWifiSetupAvailable, joinSetupNetwork, leaveSetupNetwork, portalOverWifi,
} from '../provisioning/transport';
import {ProvisionStage, PortalInfo, completeProvisioning, confirmProvisioned, readPortal}
  from '../provisioning/wizard';
import {ConnectOutcome, describeOutcome} from '../provisioning/flow';

const SETUP_PREFIX = 'StickBox-Setup-';

type Step =
  | {k: 'intro'}
  | {k: 'joining'}
  | {k: 'reading'}
  | {k: 'pick'; info: PortalInfo}
  | {k: 'password'; info: PortalInfo; net: PortalNetwork}
  | {k: 'working'; stage: ProvisionStage}
  | {k: 'result'; outcome: ConnectOutcome; dev: string; reset: boolean; confirmed?: boolean};

const STAGE_TEXT: Record<ProvisionStage, string> = {
  enrolling: '正在为设备申请密钥',
  rejoining: '正在重新连回设备热点',
  sending: '正在把 Wi-Fi 和密钥写进设备',
  waiting: '设备正在试连，别切走',
};

export function ProvisionScreen({onDone}: {onDone: () => void}) {
  const {c} = useTheme();
  const insets = useSafeAreaInsets();
  const session = useSession();

  const [step, setStep] = useState<Step>({k: 'intro'});
  const [error, setError] = useState<string | null>(null);
  const [pass, setPass] = useState('');
  const [name, setName] = useState('');

  // 门户请求绑到配网热点上；默认网络留给 enroll 走蜂窝或家里的 Wi-Fi。
  const portal = portalOverWifi(PORTAL_BASE_URL);

  // 设备验证通过后 3 秒就重启，热点消失。不放开这个网络请求的话，
  // 系统会一直替我们守着一个不存在的网络。
  useEffect(() => () => leaveSetupNetwork(), []);

  const fail = (e: unknown) => {
    if (e instanceof PortalFailure) {
      setError(
        e.error.kind === 'unreachable' ? '连不上设备。确认手机连的是设备的配网热点。'
        : e.error.kind === 'timeout' ? '设备没回应。离近一点再试。'
        : `设备回了意料之外的内容：${e.error.detail}`,
      );
    } else if (e instanceof ApiFailure) {
      setError(
        e.error.kind === 'network'
          // 配网热点没有互联网，这一步要靠蜂窝兜底。
          ? '换密钥时连不上服务器。配网过程中手机需要能上网（蜂窝数据打开）。'
          : describeApiError(e.error),
      );
    } else {
      setError((e as Error).message);
    }
  };

  const read = useCallback(async () => {
    setError(null);
    setStep({k: 'joining'});
    try {
      if (!isWifiSetupAvailable()) throw new Error('这个版本没有原生 Wi-Fi 模块');
      // 系统会弹一个只列出 StickBox-Setup 开头的网络的选择框。
      await joinSetupNetwork(SETUP_PREFIX);
      setStep({k: 'reading'});
      const info = await readPortal(portal);

      // 读完就走。手机同一时刻只能连一个 AP，赖在热点上意味着断开原来的
      // Wi-Fi，接下来的 enroll 就只能指望蜂窝——没 SIM、没开数据、
      // Wi-Fi-only 的平板全都过不去。这几秒的等待换的是不依赖蜂窝。
      leaveSetupNetwork();

      setName(`打印机 ${info.dev.slice(-4).toUpperCase()}`);
      setStep({k: 'pick', info});
    } catch (e) {
      fail(e);
      setStep({k: 'intro'});
    }
  }, []);

  const run = async (info: PortalInfo, net: PortalNetwork) => {
    setError(null);
    setStep({k: 'working', stage: 'enrolling'});
    try {
      const r = await completeProvisioning(
        session.getState().config(),
        {dev: info.dev, ssid: net.s, pass, name: name.trim() || '未命名设备'},
        portal,
        stage => setStep({k: 'working', stage}),
        // enroll 之后才连回热点。系统会再弹一次确认框——这是两次连接换
        // 「不依赖蜂窝」的代价，界面上如实说了。
        () => joinSetupNetwork(SETUP_PREFIX),
      );
      // 设备已经重启，热点没了，先放开那个网络请求。
      leaveSetupNetwork();
      setStep({k: 'result', outcome: r.outcome, dev: info.dev, reset: r.reset});
    } catch (e) {
      leaveSetupNetwork();   // 失败也要放开，否则系统一直替我们守着这个网络
      fail(e);
      setStep({k: 'pick', info});
    }
  };

  // lost / timeout 时，设备列表是唯一能回答「到底配好没有」的地方。
  useEffect(() => {
    if (step.k !== 'result') return;
    if (step.outcome.kind === 'connected' || step.confirmed !== undefined) return;
    let alive = true;
    confirmProvisioned(session.getState().config(), step.dev).then(ok => {
      if (alive) setStep(s => (s.k === 'result' ? {...s, confirmed: ok} : s));
    });
    return () => {
      alive = false;
    };
  }, [step, session]);

  return (
    <KeyboardAvoidingView
      style={[styles.fill, {backgroundColor: c.paper}]}
      behavior={Platform.OS === 'ios' ? 'padding' : undefined}>
      <ScrollView
        contentContainerStyle={[styles.body, {paddingBottom: insets.bottom + space.xl}]}
        keyboardShouldPersistTaps="handled">
        {step.k === 'intro' && (
          <>
            <Text style={[type.title, {color: c.ink}]}>添加设备</Text>
            <Text style={[type.body, styles.lede, {color: c.inkMuted}]}>
              给设备上电，它会开一个名字以 StickBox-Setup 开头的热点。
              点下面的按钮，系统会让你确认加入哪一台。
            </Text>
            <Text style={[type.label, styles.hint, {color: c.inkFaint}]}>
              过程中会连两次热点：第一次读设备信息，然后**断开去申请密钥**，
              再连第二次把 Wi-Fi 和密钥一起写进去。系统会各弹一次确认框。
              这么绕是因为连着热点时手机上不了网，申请密钥必须在断开之后做。
              密钥由 App 自动申请并写入，你不用记也不用抄。
            </Text>
            <View style={styles.actions}>
              <Button title="搜索并加入设备热点" onPress={read} />
            </View>
          </>
        )}

        {step.k === 'joining' && <Waiting text="正在加入设备的配网热点" />}
        {step.k === 'reading' && <Waiting text="正在读取设备信息" />}

        {step.k === 'pick' && (
          <>
            <Text style={[type.title, {color: c.ink}]}>选一个网络</Text>
            <Text style={[type.label, styles.hint, {color: c.inkFaint}]}>
              下面是<Text style={{color: c.inkMuted}}>设备</Text>扫到的网络，不是手机扫到的
              ——手机能看到的它未必够得着。
            </Text>
            <Section title={`设备 ${step.info.dev}`}>
              {step.info.networks.map(n => (
                <Pressable
                  key={n.s}
                  accessibilityRole="button"
                  android_ripple={{color: 'transparent'}}
                  onPress={() => {
                    setPass('');
                    setStep(n.k ? {k: 'password', info: step.info, net: n} : {k: 'password', info: step.info, net: n});
                  }}>
                  <Row label={n.k ? '需要密码' : '开放'} value={n.s} tone="normal" />
                </Pressable>
              ))}
            </Section>
          </>
        )}

        {step.k === 'password' && (
          <>
            <Text style={[type.title, {color: c.ink}]} numberOfLines={2}>
              {step.net.s}
            </Text>
            <View style={styles.form}>
              {!!step.net.k && (
                <Field
                  value={pass}
                  onChangeText={setPass}
                  placeholder="Wi-Fi 密码"
                  secureTextEntry
                  autoCapitalize="none"
                  accessibilityLabel="Wi-Fi 密码"
                />
              )}
              <Field
                value={name}
                onChangeText={setName}
                placeholder="给这台设备起个名字"
                accessibilityLabel="设备名字"
              />
              <Button
                title="开始配网"
                onPress={() => run(step.info, step.net)}
                disabled={!!step.net.k && pass.length === 0}
              />
              <Button title="换一个网络" variant="quiet"
                      onPress={() => setStep({k: 'pick', info: step.info})} />
            </View>
          </>
        )}

        {step.k === 'working' && <Waiting text={STAGE_TEXT[step.stage]} />}

        {step.k === 'result' && (
          <Result step={step} onDone={onDone} onRetry={() => setStep({k: 'intro'})} />
        )}

        {!!error && (
          <Text style={[type.body, styles.error, {color: c.accent}]}>{error}</Text>
        )}
      </ScrollView>
    </KeyboardAvoidingView>
  );
}

function Waiting({text}: {text: string}) {
  const {c} = useTheme();
  return (
    <View style={styles.center}>
      <ActivityIndicator color={c.inkMuted} />
      <Text style={[type.body, styles.waitText, {color: c.inkMuted}]}>{text}</Text>
    </View>
  );
}

function Result({
  step, onDone, onRetry,
}: {
  step: Extract<Step, {k: 'result'}>;
  onDone: () => void;
  onRetry: () => void;
}) {
  const {c} = useTheme();
  const d = describeOutcome(step.outcome);
  const ok = step.outcome.kind === 'connected' || step.confirmed === true;

  return (
    <>
      <Text style={[type.title, {color: ok ? c.ink : c.accent}]}>
        {ok ? '配好了' : d.text}
      </Text>
      {step.reset && (
        <Text style={[type.body, styles.lede, {color: c.inkMuted}]}>
          这台设备之前就绑在你的账号上，旧密钥已经作废，作业历史保留。
        </Text>
      )}
      {!ok && (
        <Text style={[type.body, styles.lede, {color: c.inkMuted}]}>{d.nextStep}</Text>
      )}
      {step.outcome.kind !== 'connected' && step.confirmed === undefined && (
        <Text style={[type.label, styles.hint, {color: c.inkFaint}]}>
          正在向服务器确认……
        </Text>
      )}
      {step.confirmed === true && step.outcome.kind !== 'connected' && (
        <Text style={[type.body, styles.lede, {color: c.inkMuted}]}>
          服务器上已经有这台设备了，配网是成功的。
        </Text>
      )}
      <Text style={[type.label, styles.dev, {color: c.inkFaint}]}>{step.dev}</Text>
      <View style={styles.actions}>
        <Button title="回到设备列表" onPress={onDone} />
        {!ok && <Button title="重新配一次" variant="quiet" onPress={onRetry} />}
      </View>
    </>
  );
}

const styles = StyleSheet.create({
  fill: {flex: 1},
  body: {paddingHorizontal: space.lg, paddingTop: space.lg, flexGrow: 1},
  lede: {marginTop: space.md},
  hint: {marginTop: space.md, lineHeight: 20},
  form: {marginTop: space.xl, gap: space.md},
  actions: {marginTop: space.xl, gap: space.md},
  center: {alignItems: 'center', paddingTop: space.xxl},
  waitText: {marginTop: space.md, textAlign: 'center'},
  error: {marginTop: space.xl},
  dev: {marginTop: space.lg, fontFamily: LATIN_FONT, letterSpacing: 1.2},
});
