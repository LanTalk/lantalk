//go:build windows

package main

import sqdialog "github.com/sqweek/dialog"

func pickFilePathWindows(imageOnly bool) (string, error) {
	file := sqdialog.File()
	if imageOnly {
		file = file.Filter("Image files", "png", "jpg", "jpeg", "gif", "bmp", "webp")
	}
	return file.Load()
}
