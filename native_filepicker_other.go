//go:build !windows

package main

import "errors"

func pickFilePathWindows(imageOnly bool) (string, error) {
	_ = imageOnly
	return "", errors.New("windows only")
}
