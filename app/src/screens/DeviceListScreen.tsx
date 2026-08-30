import React, {useCallback, useEffect, useState} from 'react';
import {ActivityIndicator, FlatList, RefreshControl, StyleSheet, Text, View} from 'react-native';
import {useSafeAreaInsets} from 'react-native-safe-area-context';
import {ApiFailure, DeviceListItem, describeApiError, listDevices} from '../api';
import {Button} from '../ui/components/Button';
import {ConnectionLine, LinkBreak} from '../ui/components/ConnectionLine';
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

function DeviceRow({d}: {d: DeviceListItem}) {
  const {c} = useTheme();
  const broken = linkBreakOf(d);

  const status =
    broken === 'bridge' ? '打印桥离线'
    : broken === 'printer' ? '没插打印机'
    : d.printer!.model;

  // 现在是静态行，不是 Pressable。设备详情页要到下一阶段才有，一个按下去有
  // scale 反馈却毫无结果的行是错的；而且 Android 会给首个可聚焦视图画一层
  // 12% 白的焦点高亮，在这套设计语言里正好读成「一张被选中的卡片」——
  // 而 DESIGN.md 明说不要卡片。详情页落地时再换回 Pressable。
  return (
    <View accessible accessibilityLabel={`${d.name}，${status}`} style={styles.row}>
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
    </View>
  );
}

export function DeviceListScreen() {
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

  useEffect(() => {
    load();
  }, [load]);

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
        renderItem={({item}) => <DeviceRow d={item} />}
        ItemSeparatorComponent={() => (
          <View style={[styles.rule, {backgroundColor: c.rule}]} />
        )}
        refreshControl={
          <RefreshControl refreshing={refreshing} onRefresh={onRefresh} tintColor={c.inkMuted} />
        }
        contentContainerStyle={{paddingBottom: insets.bottom + space.xl}}
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
                  给设备上电，连上它的 AirPrint-Setup 热点开始配网
                </Text>
              )}
            </View>
          )
        }
      />

      <View style={[styles.footer, {borderTopColor: c.rule, paddingBottom: insets.bottom + space.md}]}>
        <Button title="退出登录" variant="quiet" onPress={() => session.getState().signOut()} />
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
  footer: {borderTopWidth: StyleSheet.hairlineWidth, paddingHorizontal: space.lg, paddingTop: space.md},
});
