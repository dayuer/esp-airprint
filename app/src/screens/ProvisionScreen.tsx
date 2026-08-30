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
import {
  EnrolledKey, PortalInfo, ProvisionStage, confirmProvisioned,
  enrollForProvisioning, readPortal, writeAndWait,
} from '../provisioning/wizard';
import {ConnectOutcome, describeOutcome} from '../provisioning/flow';

const SETUP_PREFIX = 'StickBox-Setup-';

type Step =
  | {k: 'intro'}
  /** 拿密钥。**必须在连热点之前**——连上之后手机就没有互联网了。 */
  | {k: 'enrolling'}
  | {k: 'connecting'; key: EnrolledKey}
  | {k: 'reading'; key: EnrolledKey}
  | {k: 'pick'; key: EnrolledKey; info: PortalInfo}
  | {k: 'password'; key: EnrolledKey; info: PortalInfo; net: PortalNetwork}
  | {k: 'working'; stage: ProvisionStage}
  | {k: 'result'; outcome: ConnectOutcome; dev: string; reset: boolean; confirmed?: boolean};

const STAGE_TEXT: Record<ProvisionStage, string> = {
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
    try {
      // ① 先拿密钥。这一步必须在连热点之前——手机同一时刻只能连一个 AP，
      //    连上配网热点就断开了原来的 Wi-Fi，那之后 enroll 只能指望蜂窝。
      //    不带 dev：这一刻还不知道设备的 MAC，也不需要知道。
      setStep({k: 'enrolling'});
      const key = await enrollForProvisioning(
        session.getState().config(),
        name.trim() || '新设备',
      );

      // ② 连热点。唯一一次。
      setStep({k: 'connecting', key});
      if (!isWifiSetupAvailable()) throw new Error('这个版本没有原生 Wi-Fi 模块');
      await joinSetupNetwork(SETUP_PREFIX);

      // ③ 读设备信息和它扫到的网络。此时已经没有互联网了，但这些都走热点。
      setStep({k: 'reading', key});
      const info = await readPortal(portal);
      setStep({k: 'pick', key, info});
    } catch (e) {
      leaveSetupNetwork();
      fail(e);
      setStep({k: 'intro'});
    }
  }, [name]);

  const run = async (key: EnrolledKey, info: PortalInfo, net: PortalNetwork) => {
    setError(null);
    setStep({k: 'working', stage: 'sending'});
    try {
      // ④ 密钥早就在手上了，这里只写不申请。
      const outcome = await writeAndWait(
        {ssid: net.s, pass, devKey: key.deviceKey},
        portal,
        stage => setStep({k: 'working', stage}),
      );
      // 设备已经重启，热点没了，先放开那个网络请求。
      leaveSetupNetwork();
      setStep({k: 'result', outcome, dev: info.dev, reset: key.reset});
    } catch (e) {
      leaveSetupNetwork();
      fail(e);
      setStep({k: 'pick', key, info});
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
              全程只连一次热点。写完之后设备会重启、热点消失，手机自动回到
              原来的网络，设备连上后就出现在列表里。
            </Text>
            <View style={styles.form}>
              <Field
                value={name}
                onChangeText={setName}
                placeholder="给这台设备起个名字"
                accessibilityLabel="设备名字"
              />
              {/* 名字在这里问，是因为申请密钥要带着它，而密钥必须在连热点之前
                  拿到（连上热点手机就没有互联网了）。这个顺序是内部实现，
                  按钮上不提——用户要做的事就是「加入设备热点」。 */}
              <Button title="加入设备热点" onPress={read} />
            </View>
          </>
        )}

        {/* 这一步在连热点之前跑，失败信息要说清是服务器那头的问题。 */}
        {step.k === 'enrolling' && <Waiting text="正在准备" />}
        {step.k === 'connecting' && <Waiting text="正在加入设备的配网热点" />}
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
                    setStep({k: 'password', key: step.key, info: step.info, net: n});
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
              <Button
                title="开始配网"
                onPress={() => run(step.key, step.info, step.net)}
                disabled={!!step.net.k && pass.length === 0}
              />
              <Button title="换一个网络" variant="quiet"
                      onPress={() => setStep({k: 'pick', key: step.key, info: step.info})} />
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
