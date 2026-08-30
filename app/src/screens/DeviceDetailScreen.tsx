import React, {useCallback, useEffect, useState} from 'react';
import {ActivityIndicator, Alert, ScrollView, StyleSheet, Text, View} from 'react-native';
import {useSafeAreaInsets} from 'react-native-safe-area-context';
import {
  ApiFailure, DeviceListItem, JobSummary, PrinterDetail, PrinterList, ProfileStep,
  StatusResponse,
  describeApiError, describeProfileSrc, getPrinter, getStatus, listPrinters,
  shouldSuggestTest, unbindDevice,
} from '../api';
import {Button} from '../ui/components/Button';
import {ConnectionLine} from '../ui/components/ConnectionLine';
import {Row, Section} from '../ui/components/Section';
import {space, type} from '../ui/theme';
import {useTheme} from '../ui/useTheme';
import {useSession} from '../auth/context';
import {linkBreakOf} from './DeviceListScreen';

const JOB_STATE_TEXT: Record<JobSummary['state'], string> = {
  queued: '排队中',
  downloading: '设备取件中',
  printing: '打印中',
  done: '已完成',
  failed: '失败',
};

/**
 * 现在真正插着的那台是不是就是这份档案对应的那台。
 *
 * 服务端的 GET /api/device/{dev}/printer 在没插打印机时会**退回最近插过的
 * 那台**，所以拿到 printer 不等于它现在插着。判据只有一个：
 * /api/status 里的 serial（当前插着的，没插时是空串）。
 *
 * 分不清的后果是用户照着一台不在场的打印机排查——而设备面板上明明写着
 * 「打印机没连接」，两边说法对不上，没人知道该信哪个。
 */
export function isPrinterAttached(
  attachedSerial: string | undefined,
  printerSerial: string | undefined,
): boolean {
  return !!attachedSerial && !!printerSerial && attachedSerial === printerSerial;
}

/**
 * 把一串档案原语说成人话。
 *
 * 空数组和「没有这个钩子」要分开：空数组是**明确测出来「这一步不需要」**，
 * 而没有这个钩子是「按默认走」。对用户是两回事——前者是已经校准过的结论。
 */
export function describeSteps(steps?: ProfileStep[]): string {
  if (!steps) return '按默认';
  if (steps.length === 0) return '不做任何动作';
  return steps
    .map(s =>
      s.op === 'send_hex' ? `发送 ${(s.data ?? '').length / 2} 字节`
      : s.op === 'delay_ms' ? `等待 ${s.ms ?? 0}ms`
      : s.op === 'iface_reset' ? '复位接口'
      : s.op === 'read_status' ? '读状态'
      : s.op,
    )
    .join(' → ');
}

/** 作业体积：URF 是光栅，动辄几 MB，按字节显示没人看得懂。 */
export function formatBytes(n: number): string {
  if (n < 1024) return `${n} B`;
  if (n < 1024 * 1024) return `${(n / 1024).toFixed(0)} KB`;
  return `${(n / 1024 / 1024).toFixed(1)} MB`;
}

export function DeviceDetailScreen({
  device,
  onUnbound,
  onRunTests,
}: {
  device: DeviceListItem;
  onUnbound: () => void;
  onRunTests: (dev: string) => void;
}) {
  const {c} = useTheme();
  const insets = useSafeAreaInsets();
  const session = useSession();

  const [status, setStatus] = useState<StatusResponse | null>(null);
  const [printer, setPrinter] = useState<PrinterDetail | null>(null);
  const [printers, setPrinters] = useState<PrinterList | null>(null);
  const [error, setError] = useState<string | null>(null);

  const load = useCallback(async () => {
    const cfg = session.getState().config();
    try {
      setStatus(await getStatus(cfg, device.dev));
    } catch (e) {
      setError(e instanceof ApiFailure ? describeApiError(e.error) : '加载失败');
      return;
    }
    // 打印机相关的 404 是正常状态（没插打印机 / 还没枚举完），不是错误。
    try {
      setPrinter(await getPrinter(cfg, device.dev));
    } catch {
      setPrinter(null);
    }
    try {
      setPrinters(await listPrinters(cfg, device.dev));
    } catch {
      setPrinters(null);
    }
  }, [session, device.dev]);

  useEffect(() => {
    load();
  }, [load]);

  const confirmUnbind = () => {
    Alert.alert(
      '解绑这台设备？',
      '解绑后设备的密钥全部失效，需要重新配网。作业历史会保留。',
      [
        {text: '取消', style: 'cancel'},
        {
          text: '解绑',
          style: 'destructive',
          onPress: async () => {
            try {
              await unbindDevice(session.getState().config(), device.dev);
              onUnbound();
            } catch (e) {
              Alert.alert(e instanceof ApiFailure ? describeApiError(e.error) : '解绑失败');
            }
          },
        },
      ],
    );
  };

  // 当前真正插着的那台。/api/status 的 serial 是权威——空串就是没插。
  const attachedSerial = status?.device.serial ?? '';
  // handlePrinter 在没插打印机时会退回「最近插过的那台」，所以拿到 printer
  // 不等于现在插着。把陈旧状态当成当前状态显示，正是这个 App 最不该犯的错：
  // 用户会照着一台不在场的打印机去排查。
  const printerIsAttached = isPrinterAttached(attachedSerial, printer?.printer.serial);

  const broken = linkBreakOf(
    status
      ? {
          ...device,
          online: status.device.online,
          printer: printerIsAttached ? device.printer : null,
        }
      : device,
  );
  const jobs = status?.jobs ?? [];
  const suggestTest =
    printer && shouldSuggestTest(printer.profile.src, printer.profile.disputed ?? false);

  return (
    <ScrollView
      style={[styles.fill, {backgroundColor: c.paper}]}
      contentContainerStyle={{paddingBottom: insets.bottom + space.xxl}}>
      <View style={styles.head}>
        <Text style={[type.title, {color: c.ink}]}>{device.name || '未命名设备'}</Text>
        <Text style={[type.ident, styles.dev, {color: c.inkFaint}]}>{device.dev}</Text>
        <View style={styles.line}>
          <ConnectionLine broken={broken} />
        </View>
      </View>

      {!!error && (
        <Text style={[type.body, styles.error, {color: c.accent}]}>{error}</Text>
      )}

      {status === null && !error ? (
        <ActivityIndicator color={c.inkMuted} style={styles.loading} />
      ) : null}

      {status && (
        <Section title="设备">
          <Row label="状态" value={status.device.online ? '在线' : '离线'}
               tone={status.device.online ? 'normal' : 'accent'} />
          {!!status.device.prn && (
            <>
              <Row label="面板" value={status.device.prn.display} ident />
              {status.device.prn.paper_out && <Row label="纸盒" value="缺纸" tone="accent" />}
              {status.device.prn.error && <Row label="故障" value="打印机报错" tone="accent" />}
              {status.device.prn.asleep && <Row label="电源" value="休眠中" tone="muted" />}
            </>
          )}
        </Section>
      )}

      {!printerIsAttached && (
        <Section title="打印机">
          <Row label="当前" value="没插打印机" tone="accent" />
          {!!printer && (
            <Text style={[type.label, styles.note, {color: c.inkMuted}]}>
              下面是这个桥上次插的那台。适配档案按打印机序列号存，插回去就还认它。
            </Text>
          )}
        </Section>
      )}

      {printer ? (
        <Section title={printerIsAttached ? '打印机' : '上次插的打印机'}>
          <Row label="型号" value={printer.printer.model} />
          <Row label="序列号" value={printer.printer.serial} ident />
          <Row label="能力" value={printer.printer.cmd} ident />
          <Row label="光栅参数" value={printer.printer.urf_caps} ident />
        </Section>
      ) : null}

      {printer && (
        <Section title="适配档案">
          <Row
            label="来源"
            value={describeProfileSrc(printer.profile.src)}
            tone={suggestTest ? 'accent' : 'normal'}
          />
          {!!printer.profile.margins_mm && (
            <Row label="页边距" value={`${printer.profile.margins_mm.join(' / ')} mm`} ident />
          )}
          {/* 档案是一份动作序列（文档 3.7b），不是参数表。把它摊开给用户看，
              比一堆 uel_xxx 开关更接近真相——那些开关是 v1 的遗留形状。 */}
          <Row
            label="作业开始"
            value={describeSteps(printer.profile.hooks.job_begin)}
            ident
          />
          <Row label="作业结束" value={describeSteps(printer.profile.hooks.job_end)} ident />
          <Row label="唤醒" value={describeSteps(printer.profile.hooks.wake)} ident />
          {printer.profile.disputed && (
            <Text style={[type.label, styles.note, {color: c.accent}]}>
              同型号出现过相反的测试结论，已回退到更保守的配置。做一次测试能定下来。
            </Text>
          )}
          {suggestTest && !printer.profile.disputed && (
            <Text style={[type.label, styles.note, {color: c.inkMuted}]}>
              这份配置不是针对你这台机器测出来的。连打两份能验出最常见的那个问题：
              不发作业结束符时第二份不会出纸。
            </Text>
          )}
          <View style={styles.testBtn}>
            <Button
              title="做适配测试"
              variant={suggestTest ? 'primary' : 'quiet'}
              onPress={() => onRunTests(device.dev)}
            />
          </View>
        </Section>
      )}

      {printers && printers.printers.length > 1 && (
        <Section title="这个桥见过的打印机">
          {printers.printers.map(p => (
            <Row
              key={p.serial}
              label={p.attached ? '当前' : '曾用'}
              value={
                p.queued_jobs > 0
                  ? `${p.model} · 插上后自动打印 ${p.queued_jobs} 份`
                  : p.model
              }
              tone={p.queued_jobs > 0 ? 'accent' : p.attached ? 'normal' : 'muted'}
            />
          ))}
        </Section>
      )}

      <Section title="最近作业">
        {jobs.length === 0 ? (
          <Row label="" value="还没有作业" tone="muted" />
        ) : (
          jobs.map(j => (
            <Row
              key={j.id}
              label={JOB_STATE_TEXT[j.state]}
              value={`${j.name || '未命名'} · ${formatBytes(j.size)}`}
              tone={j.state === 'failed' ? 'accent' : 'normal'}
            />
          ))
        )}
      </Section>

      <View style={styles.actions}>
        <Button title="解绑设备" variant="destructive" onPress={confirmUnbind} />
        <Text style={[type.label, styles.legal, {color: c.inkFaint}]}>
          转让二手设备前要先解绑，否则新持有人绑不上。
        </Text>
      </View>
    </ScrollView>
  );
}

const styles = StyleSheet.create({
  fill: {flex: 1},
  head: {paddingHorizontal: space.lg, paddingTop: space.md},
  dev: {marginTop: space.xs},
  line: {marginTop: space.lg},
  error: {paddingHorizontal: space.lg, paddingTop: space.lg},
  loading: {marginTop: space.xxl},
  note: {paddingHorizontal: space.lg, paddingBottom: space.md, lineHeight: 20},
  testBtn: {paddingHorizontal: space.lg, paddingTop: space.sm, paddingBottom: space.md},
  actions: {paddingHorizontal: space.lg, marginTop: space.xxl},
  legal: {marginTop: space.md, lineHeight: 18},
});
