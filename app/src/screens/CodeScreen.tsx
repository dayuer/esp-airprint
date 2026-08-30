import React, {useEffect, useState} from 'react';
import {KeyboardAvoidingView, Platform, ScrollView, StyleSheet, Text, View} from 'react-native';
import {useSafeAreaInsets} from 'react-native-safe-area-context';
import {Button} from '../ui/components/Button';
import {Field} from '../ui/components/Field';
import {space, type} from '../ui/theme';
import {useTheme} from '../ui/useTheme';
import {CODE_LENGTH, formatPhone, isCompleteCode} from '../auth/phone';
import {useSession} from '../auth/context';

export function CodeScreen({phone, onBack}: {phone: string; onBack: () => void}) {
  const {c} = useTheme();
  const insets = useSafeAreaInsets();
  const session = useSession();
  const {lastError, smsCooldown} = session();
  const {submitCode, requestCode, clearError, tickCooldown} = session.getState();

  const [code, setCode] = useState('');
  const [busy, setBusy] = useState(false);

  useEffect(() => {
    const t = setInterval(tickCooldown, 1000);
    return () => clearInterval(t);
  }, [tickCooldown]);

  const submit = async (value: string) => {
    setBusy(true);
    await submitCode(phone, value, Platform.OS === 'ios' ? 'iPhone' : 'Android');
    setBusy(false);
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
        <Text style={[type.title, {color: c.ink}]}>输入验证码</Text>
        <Text style={[type.body, styles.lede, {color: c.inkMuted}]}>
          已发送到 {formatPhone(phone)}
        </Text>

        <View style={styles.form}>
          <Field
            latin
            value={code}
            onChangeText={t => {
              const digits = t.replace(/\D/g, '').slice(0, CODE_LENGTH);
              setCode(digits);
              if (lastError) clearError();
              if (isCompleteCode(digits)) submit(digits);
            }}
            placeholder="6 位数字"
            keyboardType="number-pad"
            textContentType="oneTimeCode"
            autoComplete="sms-otp"
            autoFocus
            maxLength={CODE_LENGTH}
            accessibilityLabel="验证码"
          />

          <View style={styles.errorSlot}>
            {!!lastError && <Text style={[type.label, {color: c.accent}]}>{lastError}</Text>}
          </View>

          <Button
            title={smsCooldown > 0 ? `${smsCooldown} 秒后可重发` : '重新发送'}
            variant="quiet"
            onPress={() => requestCode(phone)}
            disabled={smsCooldown > 0 || busy}
          />
          <Button title="换个号码" variant="quiet" onPress={onBack} disabled={busy} />
        </View>
      </ScrollView>
    </KeyboardAvoidingView>
  );
}

const styles = StyleSheet.create({
  fill: {flex: 1},
  body: {paddingHorizontal: space.lg, flexGrow: 1},
  lede: {marginTop: space.sm},
  form: {marginTop: space.xl, gap: space.md},
  errorSlot: {minHeight: 18},
});
