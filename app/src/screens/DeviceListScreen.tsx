import React, {useCallback, useState} from 'react';
import {useFocusEffect} from '@react-navigation/native';
import {ActivityIndicator, FlatList, RefreshControl, StyleSheet, Text, View} from 'react-native';
import {useSafeAreaInsets} from 'react-native-safe-area-context';
import {ApiFailure, DeviceListItem, describeApiError, listDevices} from '../api';
import {Button} from '../ui/components/Button';
import {ConnectionLine, LinkBreak} from '../ui/components/ConnectionLine';
import {Pressable} from '../ui/components/Pressable';
import {RasterKit} from '../native/rasterKit';
import {space, type} from '../ui/theme';
import {useTheme} from '../ui/useTheme';
import {useSession} from '../auth/context';

/**
 * 断在哪一环。
 *
 * 服务端的 `offline` 既表示「桥断了」也表示「桥在但没插打印机」——它不区分，
 * 因为从派件角度看两者一样（都不能接活）。但用户必须能区分：前者要去看设备，
 * 后者只要把 USB 插上。
 */
export function linkBreakOf(d: DeviceListItem): LinkBreak {
  if (!d.online) return 'bridge';
  if (!d.printer || !d.printer.attached) return 'printer';
  return 'none';
}

function DeviceRow({d, onPress}: {d: DeviceListItem; onPress: () => void}) {
  const {c} = useTheme();
  const broken = linkBreakOf(d);

  const status =
    broken === 'bridge' ? '打印桥离线'
    : broken === 'printer' ? '没插打印机'
    : d.printer!.model;

  return (
    <Pressable
      accessibilityRole="button"
      accessibilityLabel={`${d.name}，${status}`}
      // 关掉 Android 的默认涟漪与焦点高亮：它是一块 12% 的白/黑，
      // 在这套设计语言里正好读成「一张被选中的卡片」，而 DESIGN.md 明说不要卡片。
      // 按压反馈由 Pressable 自己的 scale(0.96) 给。
      android_ripple={{color: 'transparent', borderless: false}}
      onPress={onPress}
      style={styles.row}>
      <View style={styles.rowHead}>
        <Text style={[type.body, styles.name, {color: c.ink}]} numberOfLines={1}>
          {d.name || '未命名设备'}
        </Text>
        <Text style={[type.ident, {color: c.inkFaint}]}>{d.dev}</Text>
      </View>

      <Text
        style={[type.label, styles.status, {color: broken === 'none' ? c.inkMuted : c.accent}]}
        numberOfLines={1}>
        {status}
      </Text>

      <View style={styles.line}>
        <ConnectionLine broken={broken} />
      </View>

      {d.queued_jobs > 0 && (
        <Text style={[type.label, styles.queued, {color: c.inkMuted}]}>
          {d.queued_jobs} 份排队中
          {broken !== 'none' ? '，接上后自动打印' : ''}
        </Text>
      )}
    </Pressable>
  );
}

/** 原生模块没装上时不要崩——那会让整个设备列表白屏。 */
function encoderVersionSafe(): string {
  try {
    return RasterKit.version();
  } catch (e) {
    return `原生光栅模块未加载：${(e as Error).message}`;
  }
}

export function DeviceListScreen({
  onOpen,
  onAdd,
}: {
  onOpen: (d: DeviceListItem) => void;
  onAdd: () => void;
}) {
  const [encoderLine] = useState(encoderVersionSafe);
  const {c} = useTheme();
  const insets = useSafeAreaInsets();
  const session = useSession();

  const [devices, setDevices] = useState<DeviceListItem[] | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [refreshing, setRefreshing] = useState(false);

  const load = useCallback(async () => {
    try {
      setDevices(await listDevices(session.getState().config()));
      setError(null);
    } catch (e) {
      setError(e instanceof ApiFailure ? describeApiError(e.error) : '加载失败');
    }
  }, [session]);

  // 用 useFocusEffect 而不是 useEffect：native-stack 不会卸载栈里的页面，
  // 从配网向导 goBack() 回来时 useEffect 不会重跑——刚配好的设备就不会出现，
  // 而用户明明看到向导说「配好了」。
  useFocusEffect(
    useCallback(() => {
      load();
    }, [load]),
  );

  const onRefresh = async () => {
    setRefreshing(true);
    await load();
    setRefreshing(false);
  };

  return (
    <View style={[styles.fill, {backgroundColor: c.paper}]}>
      <FlatList
        data={devices ?? []}
        keyExtractor={d => d.dev}
        renderItem={({item}) => <DeviceRow d={item} onPress={() => onOpen(item)} />}
        ItemSeparatorComponent={() => (
          <View style={[styles.rule, {backgroundColor: c.rule}]} />
        )}
        refreshControl={
          <RefreshControl refreshing={refreshing} onRefresh={onRefresh} tintColor={c.inkMuted} />
        }
        // flexGrow 让空态也撑满，否则内容高度为 0，下拉刷新的手势够不到阈值——
        // 一个还没配网的用户配完回来就刷不出设备。
        contentContainerStyle={{flexGrow: 1, paddingBottom: insets.bottom + space.xl}}
        ListHeaderComponent={
          <View style={[styles.header, {paddingTop: insets.top + space.lg}]}>
            <Text style={[type.title, {color: c.ink}]}>我的设备</Text>
          </View>
        }
        ListEmptyComponent={
          devices === null && !error ? (
            <View style={styles.center}>
              <ActivityIndicator color={c.inkMuted} />
            </View>
          ) : (
            <View style={styles.center}>
              <Text style={[type.body, styles.emptyText, {color: c.inkMuted}]}>
                {error ?? '还没有绑定的设备'}
              </Text>
              {!error && (
                <Text style={[type.label, styles.emptyHint, {color: c.inkFaint}]}>
                  给设备上电，连上它的 StickBox-Setup 热点，然后点下面的「添加设备」
                </Text>
              )}
            </View>
          )
        }
      />

      <View style={[styles.footer, {borderTopColor: c.rule, paddingBottom: insets.bottom + space.md}]}>
        <Button title="添加设备" onPress={onAdd} />
        <Button title="退出登录" variant="quiet" onPress={() => session.getState().signOut()} />
        {/* 临时：原生光栅链路的可见凭证。阶段三做完就删。 */}
        <Text style={[type.label, styles.encoder, {color: c.inkFaint}]}>{encoderLine}</Text>
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  fill: {flex: 1},
  header: {paddingHorizontal: space.lg, paddingBottom: space.lg},
  // 没有卡片：行就是行，靠发丝线分隔
  row: {paddingHorizontal: space.lg, paddingVertical: space.lg},
  rowHead: {flexDirection: 'row', alignItems: 'baseline', justifyContent: 'space-between'},
  name: {fontWeight: '600', flexShrink: 1, marginRight: space.md},
  status: {marginTop: space.xs},
  line: {marginTop: space.md},
  queued: {marginTop: space.md},
  rule: {height: StyleSheet.hairlineWidth, marginHorizontal: space.lg},
  center: {alignItems: 'center', paddingTop: space.xxl, paddingHorizontal: space.lg},
  emptyText: {textAlign: 'center'},
  emptyHint: {textAlign: 'center', marginTop: space.sm},
  // 浮起的操作条用一条线，不是阴影
  footer: {
    borderTopWidth: StyleSheet.hairlineWidth,
    paddingHorizontal: space.lg,
    paddingTop: space.md,
    gap: space.sm,
  },
  encoder: {textAlign: 'center', marginTop: space.sm},
});
