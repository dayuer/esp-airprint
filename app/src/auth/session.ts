import {create} from 'zustand';
import {ApiFailure, ClientConfig, Token, logout as apiLogout, sendSms, verifySms} from '../api';
import {TokenStore} from './tokenStore';
import {normalizePhone} from './phone';

export type SessionPhase = 'booting' | 'signedOut' | 'signedIn';

export interface SessionState {
  phase: SessionPhase;
  token: Token | null;
  phoneTail: string | null;
  /** 还能过多少秒才可以再次发码。0 表示可以发。 */
  smsCooldown: number;
  lastError: string | null;
}

export interface SessionDeps {
  baseUrl: string;
  store: TokenStore;
  now?: () => number;
}

/** 服务端的同号码发码间隔（文档 4.1b）。429 时不自动重试，把倒计时显示给用户。 */
export const SMS_COOLDOWN_SEC = 60;

export interface SessionActions {
  bootstrap(): Promise<void>;
  requestCode(rawPhone: string): Promise<void>;
  submitCode(rawPhone: string, code: string, deviceName?: string): Promise<void>;
  signOut(all?: boolean): Promise<void>;
  tickCooldown(): void;
  clearError(): void;
  /** 给需要 token 的调用方用。未登录时 token 为 undefined。 */
  config(): ClientConfig;
}

export function createSessionStore(deps: SessionDeps) {
  const errText = (e: unknown): string => {
    if (e instanceof ApiFailure) {
      const {describeApiError} = require('../api') as typeof import('../api');
      return describeApiError(e.error);
    }
    return '出了点问题，再试一次';
  };

  return create<SessionState & SessionActions>((set, get) => ({
    phase: 'booting',
    token: null,
    phoneTail: null,
    smsCooldown: 0,
    lastError: null,

    config: () => ({baseUrl: deps.baseUrl, token: get().token ?? undefined}),

    async bootstrap() {
      const token = await deps.store.load();
      set({token, phase: token ? 'signedIn' : 'signedOut'});
    },

    async requestCode(rawPhone) {
      const phone = normalizePhone(rawPhone);
      if (!phone) {
        set({lastError: '手机号格式不对'});
        return;
      }
      try {
        await sendSms({baseUrl: deps.baseUrl}, phone);
        set({smsCooldown: SMS_COOLDOWN_SEC, lastError: null});
      } catch (e) {
        // 429 不自动重试——重试只会撞更严的闸。把倒计时显示给用户。
        if (e instanceof ApiFailure && e.error.kind === 'rateLimited') {
          set({smsCooldown: SMS_COOLDOWN_SEC, lastError: errText(e)});
          return;
        }
        set({lastError: errText(e)});
      }
    },

    async submitCode(rawPhone, code, deviceName) {
      const phone = normalizePhone(rawPhone);
      if (!phone) {
        set({lastError: '手机号格式不对'});
        return;
      }
      try {
        const r = await verifySms({baseUrl: deps.baseUrl}, phone, code, deviceName);
        await deps.store.save(r.token);
        set({token: r.token, phoneTail: r.phone_tail, phase: 'signedIn', lastError: null});
      } catch (e) {
        set({lastError: errText(e)});
      }
    },

    async signOut(all = false) {
      const {token} = get();
      if (token) {
        // 服务端登出失败不能把用户困在已登录态——本地一定要清掉。
        try {
          await apiLogout({baseUrl: deps.baseUrl, token}, all);
        } catch {
          /* 忽略 */
        }
      }
      await deps.store.clear();
      set({token: null, phoneTail: null, phase: 'signedOut', lastError: null});
    },

    tickCooldown() {
      const {smsCooldown} = get();
      if (smsCooldown > 0) set({smsCooldown: smsCooldown - 1});
    },

    clearError() {
      set({lastError: null});
    },
  }));
}
