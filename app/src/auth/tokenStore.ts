import * as Keychain from 'react-native-keychain';
import {Token} from '../api';

/**
 * session token 的存储。
 *
 * token 无过期时间（文档 4.1b），下线靠吊销，所以它会在设备上长期躺着——
 * 必须进 Keychain / Keystore，不能进 AsyncStorage。
 *
 * 抽成接口是为了让会话逻辑能在开发机上测：真实现要原生模块，内存实现不要。
 */
export interface TokenStore {
  load(): Promise<Token | null>;
  save(token: Token): Promise<void>;
  clear(): Promise<void>;
}

const SERVICE = 'id.silkline.airprint.session';
/** Keychain 的 API 要一对用户名密码，这里只用得上密码。 */
const ACCOUNT = 'session';

export const keychainTokenStore: TokenStore = {
  async load() {
    try {
      const r = await Keychain.getGenericPassword({service: SERVICE});
      return r ? r.password : null;
    } catch {
      // 钥匙串读不出来（换了设备、用户改了锁屏）等同于没登录，不该崩。
      return null;
    }
  },
  async save(token) {
    await Keychain.setGenericPassword(ACCOUNT, token, {
      service: SERVICE,
      // 只在本机可用，不同步到 iCloud——token 是这台手机的凭据。
      accessible: Keychain.ACCESSIBLE.WHEN_UNLOCKED_THIS_DEVICE_ONLY,
    });
  },
  async clear() {
    await Keychain.resetGenericPassword({service: SERVICE});
  },
};

/** 测试用。 */
export function memoryTokenStore(initial: Token | null = null): TokenStore {
  let value = initial;
  return {
    load: async () => value,
    save: async t => {
      value = t;
    },
    clear: async () => {
      value = null;
    },
  };
}
