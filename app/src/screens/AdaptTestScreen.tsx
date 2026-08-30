import React, {useCallback, useEffect, useState} from 'react';
import {ActivityIndicator, ScrollView, StyleSheet, Text, View} from 'react-native';
import {useSafeAreaInsets} from 'react-native-safe-area-context';
import {
  AdaptTest, ApiFailure, DeviceId, TestStartResponse, Verdict,
  answerTest, describeApiError, listTests, startTest,
} from '../api';
import {Button} from '../ui/components/Button';
import {Pressable} from '../ui/components/Pressable';
import {Row, Section} from '../ui/components/Section';
import {space, type} from '../ui/theme';
import {useTheme} from '../ui/useTheme';
import {useSession} from '../auth/context';

const STATE_TEXT: Record<AdaptTest['state'], string> = {
  untested: '没测过',
  running: '进行中',
  passed: '通过',
  failed: '未通过',
  unclear: '说不准',
};

/** 每个测试问用户什么。文案要具体到「看什么」，不能只问「行不行」。 */
const QUESTIONS: Record<string, {q: string; options: {label: string; verdict: Verdict; detail?: Record<string, unknown>}[]}> = {
  double_print: {
    q: '一共出了几张纸？',
    options: [
      {label: '两张都出了', verdict: 'pass', detail: {pages_printed: 2}},
      {label: '只出了一张', verdict: 'fail', detail: {pages_printed: 1}},
      {label: '说不准', verdict: 'unclear'},
    ],
  },
  wake: {
    q: '打印机醒过来出纸了吗？',
    options: [
      {label: '醒了并出纸', verdict: 'pass', detail: {printed: true}},
      {label: '没反应', verdict: 'fail', detail: {printed: false}},
      {label: '说不准', verdict: 'unclear'},
    ],
  },
};

export function AdaptTestScreen({dev}: {dev: DeviceId}) {
  const {c} = useTheme();
  const insets = useSafeAreaInsets();
  const session = useSession();

  const [tests, setTests] = useState<AdaptTest[] | null>(null);
  const [run, setRun] = useState<(TestStartResponse & {test: string}) | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);

  const load = useCallback(async () => {
    try {
      setTests(await listTests(session.getState().config(), dev));
      setError(null);
    } catch (e) {
      setError(e instanceof ApiFailure ? describeApiError(e.error) : '加载失败');
    }
  }, [session, dev]);

  useEffect(() => {
    load();
  }, [load]);

  const begin = async (t: AdaptTest) => {
    setError(null);
    setBusy(true);
    try {
      const r = await startTest(session.getState().config(), dev, t.id);
      setRun({...r, test: t.id});
    } catch (e) {
      // 通用文案在这里不够用。404 在这个页面上几乎只有一个含义——
      // 「设备上没插打印机」，服务端 handleTestStart 明确会因此拒绝。
      // 直接说「找不到对应的记录」，用户会去怀疑测试本身而不是去插线。
      if (e instanceof ApiFailure && e.error.kind === 'notFound') {
        setError('设备上没插打印机，测不了。插上再来。');
      } else if (e instanceof ApiFailure && e.error.kind === 'conflict') {
        setError(e.error.detail || '这台设备已有一轮测试在进行中');
      } else {
        setError(e instanceof ApiFailure ? describeApiError(e.error) : '开始失败');
      }
    } finally {
      setBusy(false);
    }
  };

  const answer = async (verdict: Verdict, detail?: Record<string, unknown>) => {
    if (!run) return;
    setBusy(true);
    try {
      const r = await answerTest(session.getState().config(), run.run_id, verdict, detail);
      setError(null);
      setRun(null);
      await load();
      setError(r.message);   // 复用同一块区域展示结论，它不是错误但同样要看见
    } catch (e) {
      setError(e instanceof ApiFailure ? describeApiError(e.error) : '提交失败');
    } finally {
      setBusy(false);
    }
  };

  const q = run ? QUESTIONS[run.test] : undefined;

  return (
    <ScrollView
      style={[styles.fill, {backgroundColor: c.paper}]}
      contentContainerStyle={{paddingBottom: insets.bottom + space.xxl}}>
      <View style={styles.head}>
        <Text style={[type.title, {color: c.ink}]}>适配测试</Text>
        <Text style={[type.body, styles.lede, {color: c.inkMuted}]}>
          USB 层的怪癖没法从机型资料查出来，只能打一张看一眼。
          测出来的结论只对这台打印机生效；同型号有三台得出一样的结果，
          才会成为该机型的默认配置。
        </Text>
      </View>

      {run ? (
        <Section title="正在测试">
          <Text style={[type.body, styles.instruction, {color: c.ink}]}>
            {run.instruction}
          </Text>
          {!run.baseline_ok && (
            <Text style={[type.body, styles.instruction, {color: c.accent}]}>
              {run.hint ?? '当前配置本身就打不出来，先别测变体——在坏掉的链路上测，得到的全是噪音。'}
            </Text>
          )}
          {!!q && (
            <>
              <Text style={[type.body, styles.question, {color: c.ink}]}>{q.q}</Text>
              <View style={styles.actions}>
                {q.options.map(o => (
                  <Button
                    key={o.label}
                    title={o.label}
                    variant={o.verdict === 'unclear' ? 'quiet' : 'primary'}
                    disabled={busy}
                    onPress={() => answer(o.verdict, o.detail)}
                  />
                ))}
                <Button
                  title="中止这一轮"
                  variant="quiet"
                  disabled={busy}
                  onPress={() => answer('aborted')}
                />
              </View>
              <Text style={[type.label, styles.note, {color: c.inkFaint}]}>
                不确定就选「说不准」。猜出来的数据比没有数据更糟——它会经由
                机型投票扩散到同型号所有人身上。
              </Text>
            </>
          )}
        </Section>
      ) : (
        <Section title="可做的测试">
          {tests === null ? (
            <ActivityIndicator color={c.inkMuted} style={styles.loading} />
          ) : (
            tests.map(t => (
              <Pressable
                key={t.id}
                accessibilityRole="button"
                accessibilityLabel={`${t.name}，${STATE_TEXT[t.state]}`}
                android_ripple={{color: 'transparent'}}
                disabled={busy}
                onPress={() => begin(t)}>
                <Row
                  label={STATE_TEXT[t.state]}
                  value={`${t.name}${t.required ? '（必做）' : ''} · 打 ${t.jobs_needed} 页`}
                  tone={t.state === 'failed' ? 'accent' : t.state === 'untested' ? 'normal' : 'muted'}
                />
              </Pressable>
            ))
          )}
        </Section>
      )}

      {!!error && (
        <Text style={[type.body, styles.error, {color: c.inkMuted}]}>{error}</Text>
      )}
    </ScrollView>
  );
}

const styles = StyleSheet.create({
  fill: {flex: 1},
  head: {paddingHorizontal: space.lg, paddingTop: space.md},
  lede: {marginTop: space.md, lineHeight: 24},
  instruction: {paddingHorizontal: space.lg, paddingTop: space.md, lineHeight: 24},
  question: {paddingHorizontal: space.lg, paddingTop: space.lg, fontWeight: '600'},
  actions: {paddingHorizontal: space.lg, marginTop: space.md, gap: space.sm},
  note: {paddingHorizontal: space.lg, marginTop: space.md, lineHeight: 20},
  loading: {marginVertical: space.lg},
  error: {paddingHorizontal: space.lg, marginTop: space.lg, lineHeight: 24},
});
