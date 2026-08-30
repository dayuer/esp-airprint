// Package version 只做一件事：让二进制能报出自己是哪个版本。
package version

// Version 由构建时 -ldflags 覆盖，默认值用于开发构建。
var Version = "dev"

func String() string { return Version }
