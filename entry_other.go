//go:build !windows

package main

import "fmt"

func runEntry() {
	fmt.Println("LanTalk 当前仅实现 Windows GUI，Linux/macOS 版本后续补齐。")
}
