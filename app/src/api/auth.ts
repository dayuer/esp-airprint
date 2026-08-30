import {ClientConfig, request} from './http';
import {AccountDeleteResponse, EnrollResponse, SmsResponse, VerifyResponse} from './types';

/** 4.1b 发送验证码。429 时不要自动重试，把倒计时显示给用户。 */
export function sendSms(cfg: ClientConfig, phone: string) {
  return request<SmsResponse>(cfg, {path: '/auth/sms', body: {phone}, device: null});
}

/** 4.1b 校验并登录。用户不存在则自动创建。 */
export function verifySms(cfg: ClientConfig, phone: string, code: string, device?: string) {
  return request<VerifyResponse>(cfg, {
    path: '/auth/verify',
    body: {phone, code, ...(device ? {device} : {})},
    device: null,
  });
}

/** 4.1b 登出。all=true 踢掉该用户的全部 token。 */
export function logout(cfg: ClientConfig, all = false) {
  return request<{ok: 1}>(cfg, {
    path: '/auth/logout',
    method: 'POST',
    body: {},
    device: null,
    ...(all ? {query: {all: '1'}} : {}),
  });
}

/** 4.1b 注销账号。立即执行，没有冷静期。 */
export function deleteAccount(cfg: ClientConfig) {
  return request<AccountDeleteResponse>(cfg, {
    path: '/account/delete', method: 'POST', body: {}, device: null,
  });
}

/** 4.1b 配网时为设备签发密钥。409 表示该设备属于其他账号。 */
export function enrollDevice(cfg: ClientConfig, dev: string, name: string) {
  return request<EnrollResponse>(cfg, {
    path: '/device/enroll', body: {dev, name}, device: null,
  });
}
