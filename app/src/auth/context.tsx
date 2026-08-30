import React, {createContext, useContext} from 'react';
import {createSessionStore} from './session';

type Store = ReturnType<typeof createSessionStore>;

const Ctx = createContext<Store | null>(null);

export function SessionProvider({store, children}: {store: Store; children: React.ReactNode}) {
  return <Ctx.Provider value={store}>{children}</Ctx.Provider>;
}

export function useSession(): Store {
  const s = useContext(Ctx);
  if (!s) throw new Error('useSession 必须在 SessionProvider 里用');
  return s;
}
