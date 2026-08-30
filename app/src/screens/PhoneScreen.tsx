import React, {useState} from 'react';
import {KeyboardAvoidingView, Platform, ScrollView, StyleSheet, Text, View} from 'react-native';
import {useSafeAreaInsets} from 'react-native-safe-area-context';
import {Button} from '../ui/components/Button';
import {Field} from '../ui/components/Field';
import {space, type} from '../ui/theme';
import {useTheme} from '../ui/useTheme';
import {formatPhone, isValidPhone} from '../auth/phone';
import {useSession} from '../auth/context';

export function PhoneScreen({onSent}: {onSent: (phone: string) => void}) {
  const {c} = useTheme();
  const insets = useSafeAreaInsets();
  const session = useSession();
  const {lastError, smsCooldown} = session();
  const {requestCode, clearError} = session.getState();

  const [raw, setRaw] = useState('');
  const [busy, setBusy] = useState(false);
  const ok = isValidPhone(raw);

  const submit = async () => {
    setBusy(true);
    await requestCode(raw);
    setBusy(false);
    if (!session.getState().lastError) onSent(raw);
  };

  return (
    <KeyboardAvoidingView
      style={[styles.fill, {backgroundColor: c.paper}]}
      behavior={Platform.OS === 'ios' ? 'padding' : undefined}>
      <ScrollView
        contentContainerStyle={[
          styles.body,
          {paddingTop: insets.top + space.xxl, paddingBottom: insets.bottom + space.xl},
        ]}
        keyboardShouldPersistTaps="handled">
        <Text style={[type.display, {color: c.ink}]}>把打印机{'\n'}接到网上</Text>
        <Text style={[type.body, styles.lede, {color: c.inkMuted}]}>
          用手机号登录。之后配网、投递、看状态都在这里。
        </Text>

        <View style={styles.form}>
          <Field
            latin
            value={formatPhone(raw)}
            onChangeText={t => {
              setRaw(t);
              if (lastError) clearError();
            }}
            placeholder="手机号"
            keyboardType="number-pad"
            textContentType="telephoneNumber"
            autoComplete="tel"
            maxLength={13}
            accessibilityLabel="手机号"
          />

          {/* 错误位固定高度，出现和消失都不能让下面的按钮跳位 */}
          <View style={styles.errorSlot}>
            {!!lastError && (
              <Text style={[type.label, {color: c.accent}]}>{lastError}</Text>
            )}
          </View>

          <Button
            title={smsCooldown > 0 ? `${smsCooldown} 秒后可重发` : '获取验证码'}
            onPress={submit}
            disabled={!ok || smsCooldown > 0}
            loading={busy}
          />
        </View>

        <Text style={[type.label, styles.legal, {color: c.inkFaint}]}>
          继续即表示同意《用户协议》与《隐私政策》。仅支持中国大陆手机号。
        </Text>
      </ScrollView>
    </KeyboardAvoidingView>
  );
}

const styles = StyleSheet.create({
  fill: {flex: 1},
  body: {paddingHorizontal: space.lg, flexGrow: 1},
  lede: {marginTop: space.md, maxWidth: 300},
  form: {marginTop: space.xxl, gap: space.md},
  errorSlot: {minHeight: 18},
  legal: {marginTop: 'auto', paddingTop: space.xl},
});
