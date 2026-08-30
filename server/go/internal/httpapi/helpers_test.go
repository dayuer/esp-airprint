package httpapi

import "os"

func osReadDir(dir string) ([]os.DirEntry, error) { return os.ReadDir(dir) }

// loadCapsForTest 复用生产代码的解析逻辑，避免测试自己再实现一遍。
func loadCapsForTest(dep *testDeps, dev string) (caps, serial string, ok bool) {
	return (&API{cfg: dep.cfg}).loadIdentCaps(dev)
}

func io_ReadAll(r interface{ Read([]byte) (int, error) }) ([]byte, error) {
	buf := make([]byte, 0, 512)
	tmp := make([]byte, 512)
	for {
		n, err := r.Read(tmp)
		buf = append(buf, tmp[:n]...)
		if err != nil {
			return buf, nil
		}
	}
}
