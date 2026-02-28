package main

import (
	"bufio"
	"bytes"
	"crypto/aes"
	"crypto/cipher"
	"crypto/rand"
	"crypto/sha256"
	"encoding/base64"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"image/color"
	"io"
	"net"
	"os"
	"path/filepath"
	"runtime"
	"sort"
	"strings"
	"sync"
	"time"

	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/app"
	"fyne.io/fyne/v2/canvas"
	"fyne.io/fyne/v2/container"
	"fyne.io/fyne/v2/dialog"
	"fyne.io/fyne/v2/driver/desktop"
	"fyne.io/fyne/v2/layout"
	"fyne.io/fyne/v2/storage"
	"fyne.io/fyne/v2/theme"
	"fyne.io/fyne/v2/widget"
	"golang.org/x/crypto/curve25519"
	"golang.org/x/crypto/hkdf"
	"golang.org/x/net/ipv4"
)

const (
	discoveryPort          = 43999
	discoveryHost          = "239.255.77.77"
	defaultChatPort        = 44000
	presenceTTL            = 20 * time.Second
	announceEvery          = 3 * time.Second
	autoProbeEvery         = 10 * time.Second
	scanBatchSize          = 220
	probeParallelism       = 24
	maxEnvelopeBytes       = 1 << 20
	fileChunkBytes         = 1 << 20
	maxFileBytes     int64 = 100 * 1024 * 1024 * 1024
)

type config struct {
	ID         string            `json:"id"`
	Name       string            `json:"name"`
	Avatar     string            `json:"avatar,omitempty"`
	Remarks    map[string]string `json:"remarks,omitempty"`
	PublicKey  string            `json:"publicKey"`
	PrivateKey string            `json:"privateKey"`
}

type peer struct {
	Key        string
	ID         string
	InstanceID string
	Name       string
	Addr       string
	Port       int
	PublicKey  string
	LastSeen   time.Time
}

type peerRow struct {
	Key      string
	Title    string
	Subtitle string
	Unread   int
}

type discoveryPacket struct {
	Type       string `json:"type"`
	ID         string `json:"id"`
	InstanceID string `json:"instanceId"`
	Name       string `json:"name"`
	PublicKey  string `json:"publicKey"`
	Port       int    `json:"port"`
	Time       int64  `json:"time"`
}

type plainPayload struct {
	ID        string `json:"id"`
	Kind      string `json:"kind"`
	Text      string `json:"text,omitempty"`
	FileName  string `json:"fileName,omitempty"`
	FileSize  int64  `json:"fileSize,omitempty"`
	ChunkSize int    `json:"chunkSize,omitempty"`
	Salt      string `json:"salt,omitempty"`
	SentAt    int64  `json:"sentAt"`
}

type encryptedMessage struct {
	Salt  string `json:"salt"`
	Nonce string `json:"nonce"`
	Data  string `json:"data"`
}

type chatEnvelope struct {
	Type         string           `json:"type"`
	FromID       string           `json:"fromId"`
	FromInstance string           `json:"fromInstance"`
	FromName     string           `json:"fromName"`
	FromPubKey   string           `json:"fromPubKey"`
	Port         int              `json:"port,omitempty"`
	Payload      encryptedMessage `json:"payload,omitempty"`
}

type historyLine struct {
	Direction string `json:"direction"`
	From      string `json:"from"`
	Text      string `json:"text"`
	SentAt    int64  `json:"sentAt"`
}

type oldQQTheme struct{}

type chatInput struct {
	widget.Entry
	onSend    func()
	shiftHeld bool
}

func newChatInput(onSend func()) *chatInput {
	e := &chatInput{onSend: onSend}
	e.MultiLine = true
	e.Wrapping = fyne.TextWrapWord
	e.ExtendBaseWidget(e)
	return e
}

func (e *chatInput) KeyDown(key *fyne.KeyEvent) {
	if key != nil && (key.Name == desktop.KeyShiftLeft || key.Name == desktop.KeyShiftRight) {
		e.shiftHeld = true
	}
	e.Entry.KeyDown(key)
}

func (e *chatInput) KeyUp(key *fyne.KeyEvent) {
	if key != nil && (key.Name == desktop.KeyShiftLeft || key.Name == desktop.KeyShiftRight) {
		e.shiftHeld = false
	}
	e.Entry.KeyUp(key)
}

func (e *chatInput) TypedKey(key *fyne.KeyEvent) {
	if key != nil && (key.Name == fyne.KeyReturn || key.Name == fyne.KeyEnter) && !e.shiftHeld {
		if e.onSend != nil {
			e.onSend()
		}
		return
	}
	e.Entry.TypedKey(key)
}

func (oldQQTheme) Color(name fyne.ThemeColorName, variant fyne.ThemeVariant) color.Color {
	switch name {
	case theme.ColorNamePrimary:
		return color.NRGBA{R: 53, G: 146, B: 255, A: 255}
	case theme.ColorNameBackground:
		return color.NRGBA{R: 244, G: 249, B: 255, A: 255}
	case theme.ColorNameInputBackground:
		return color.NRGBA{R: 255, G: 255, B: 255, A: 255}
	default:
		return theme.DefaultTheme().Color(name, variant)
	}
}
func (oldQQTheme) Font(style fyne.TextStyle) fyne.Resource    { return theme.DefaultTheme().Font(style) }
func (oldQQTheme) Icon(name fyne.ThemeIconName) fyne.Resource { return theme.DefaultTheme().Icon(name) }
func (oldQQTheme) Size(name fyne.ThemeSizeName) float32       { return theme.DefaultTheme().Size(name) }

type appState struct {
	baseDir        string
	dataDir        string
	chatsDir       string
	downloadsDir   string
	profileDir     string
	instanceID     string
	listenPort     int
	cfg            config
	mu             sync.Mutex
	peers          map[string]*peer
	unread         map[string]int
	peerRows       []peerRow
	peerFilter     string
	activeKey      string
	scanOffset     int
	tcpListener    net.Listener
	udpConn        *net.UDPConn
	stopOnce       sync.Once
	stopCh         chan struct{}
	uiApp          fyne.App
	win            fyne.Window
	settingsWin    fyne.Window
	contactBox     *widget.List
	searchInput    *widget.Entry
	topInfo        *widget.Label
	chatHeader     *widget.Label
	chatSubHeader  *widget.Label
	chatStream     *fyne.Container
	chatScroll     *container.Scroll
	inputBox       *chatInput
	statusBar      *widget.Label
	headerAvatar   *canvas.Image
	settingsAvatar *canvas.Image
}

func newApp(baseDir string) *appState {
	return &appState{
		baseDir:      baseDir,
		dataDir:      filepath.Join(baseDir, "data"),
		chatsDir:     filepath.Join(baseDir, "data", "chats"),
		downloadsDir: filepath.Join(baseDir, "data", "downloads"),
		profileDir:   filepath.Join(baseDir, "data", "profile"),
		instanceID:   randomID(4),
		peers:        map[string]*peer{},
		unread:       map[string]int{},
		stopCh:       make(chan struct{}),
	}
}

func runApp() error {
	exe, err := os.Executable()
	if err != nil {
		return err
	}
	return newApp(filepath.Dir(exe)).run()
}

func (a *appState) run() error {
	if err := a.initStorage(); err != nil {
		return err
	}
	if err := a.buildUI(); err != nil {
		return err
	}
	port, err := a.startTCPServer()
	if err != nil {
		return err
	}
	a.listenPort = port
	if err := a.startDiscovery(port); err != nil {
		return err
	}
	a.pushStatus("在线")
	a.win.ShowAndRun()
	a.shutdown()
	return nil
}

func (a *appState) initStorage() error {
	for _, p := range []string{a.dataDir, a.chatsDir, a.downloadsDir, a.profileDir} {
		if err := os.MkdirAll(p, 0o755); err != nil {
			return err
		}
	}
	cfgPath := filepath.Join(a.dataDir, "config.json")
	if b, err := os.ReadFile(cfgPath); err == nil {
		if err := json.Unmarshal(b, &a.cfg); err != nil {
			return err
		}
	}

	if strings.TrimSpace(a.cfg.ID) == "" {
		a.cfg.ID = newUserCode()
	}
	if strings.TrimSpace(a.cfg.Name) == "" {
		host, _ := os.Hostname()
		if strings.TrimSpace(host) == "" {
			host = "LanUser"
		}
		a.cfg.Name = host
	}
	if a.cfg.Remarks == nil {
		a.cfg.Remarks = map[string]string{}
	}
	if a.cfg.PublicKey == "" || a.cfg.PrivateKey == "" {
		pub, priv, err := generateKeyPair()
		if err != nil {
			return err
		}
		a.cfg.PublicKey = pub
		a.cfg.PrivateKey = priv
	}
	return a.saveConfig()
}

func (a *appState) saveConfig() error {
	b, err := json.MarshalIndent(a.cfg, "", "  ")
	if err != nil {
		return err
	}
	return os.WriteFile(filepath.Join(a.dataDir, "config.json"), b, 0o644)
}

func (a *appState) buildUI() error {
	a.uiApp = app.NewWithID("lantalk")
	a.uiApp.SetIcon(resourceAppIcon)
	a.uiApp.Settings().SetTheme(oldQQTheme{})
	a.win = a.uiApp.NewWindow("LanTalk")
	a.win.SetIcon(resourceAppIcon)
	a.win.Resize(fyne.NewSize(1120, 740))
	a.win.SetFixedSize(false)
	a.win.SetContent(a.buildChatPage())
	a.win.SetCloseIntercept(func() {
		a.shutdown()
		a.win.Close()
	})
	return nil
}

func (a *appState) buildChatPage() fyne.CanvasObject {
	a.searchInput = widget.NewEntry()
	a.searchInput.SetPlaceHolder("搜索好友")
	a.searchInput.OnChanged = func(v string) {
		a.mu.Lock()
		a.peerFilter = strings.TrimSpace(v)
		a.mu.Unlock()
		a.refreshContacts()
	}

	a.contactBox = widget.NewList(
		func() int {
			a.mu.Lock()
			defer a.mu.Unlock()
			return len(a.peerRows)
		},
		func() fyne.CanvasObject {
			icon := widget.NewIcon(theme.AccountIcon())
			title := widget.NewLabel("")
			title.TextStyle = fyne.TextStyle{Bold: true}
			sub := widget.NewLabel("")
			sub.Wrapping = fyne.TextWrapOff
			return container.NewHBox(icon, container.NewVBox(title, sub))
		},
		func(id widget.ListItemID, obj fyne.CanvasObject) {
			a.mu.Lock()
			row := peerRow{}
			if id >= 0 && id < len(a.peerRows) {
				row = a.peerRows[id]
			}
			a.mu.Unlock()
			c := obj.(*fyne.Container)
			icon := c.Objects[0].(*widget.Icon)
			textCol := c.Objects[1].(*fyne.Container)
			if row.Unread > 0 {
				icon.SetResource(theme.MailComposeIcon())
			} else {
				icon.SetResource(theme.AccountIcon())
			}
			textCol.Objects[0].(*widget.Label).SetText(row.Title)
			textCol.Objects[1].(*widget.Label).SetText(row.Subtitle)
		},
	)
	a.contactBox.OnSelected = func(id widget.ListItemID) { a.selectPeer(int(id)) }

	leftHeadTitle := widget.NewLabel("联系人")
	leftHeadTitle.TextStyle = fyne.TextStyle{Bold: true}
	leftPanel := container.NewVBox(leftHeadTitle, a.searchInput, a.contactBox)
	leftCard := widget.NewCard("", "", leftPanel)
	leftBg := canvas.NewRectangle(color.NRGBA{R: 233, G: 244, B: 255, A: 255})
	left := container.NewMax(leftBg, container.NewPadded(leftCard))

	a.headerAvatar = avatarImage(a.avatarAbsPath(), 52)
	a.chatHeader = widget.NewLabel("请选择联系人")
	a.chatHeader.TextStyle = fyne.TextStyle{Bold: true}
	a.chatSubHeader = widget.NewLabel("")
	remarkBtn := widget.NewButtonWithIcon("备注", theme.DocumentCreateIcon(), func() { a.openRemarkDialog() })

	a.chatStream = container.NewVBox()
	a.chatScroll = container.NewVScroll(a.chatStream)
	a.chatScroll.Direction = container.ScrollVerticalOnly

	a.inputBox = newChatInput(func() { a.sendCurrentText() })
	a.inputBox.SetMinRowsVisible(6)
	a.inputBox.SetPlaceHolder("输入消息")

	refreshBtn := widget.NewButtonWithIcon("", theme.ViewRefreshIcon(), func() {
		a.broadcastHello(a.listenPort)
		go a.probeAutoTargets()
	})
	emojiBtn := widget.NewButtonWithIcon("表情", theme.ContentAddIcon(), func() { a.openEmojiPicker() })
	imageBtn := widget.NewButtonWithIcon("图片", theme.FileImageIcon(), func() { a.sendCurrentImage() })
	fileBtn := widget.NewButtonWithIcon("文件", theme.FileIcon(), func() { a.sendCurrentFile() })
	clearBtn := widget.NewButtonWithIcon("", theme.ContentClearIcon(), func() { a.inputBox.SetText("") })
	sendBtn := widget.NewButtonWithIcon("发送", theme.MailSendIcon(), func() { a.sendCurrentText() })
	sendBtn.Importance = widget.HighImportance
	a.statusBar = widget.NewLabel("")

	headerRow := container.NewHBox(
		a.headerAvatar,
		container.NewVBox(a.chatHeader, a.chatSubHeader),
		layout.NewSpacer(),
		remarkBtn,
	)
	chatSplit := container.NewVSplit(a.chatScroll, a.inputBox)
	chatSplit.Offset = 0.78
	actions := container.NewHBox(refreshBtn, emojiBtn, imageBtn, fileBtn, clearBtn, layout.NewSpacer(), sendBtn)
	rightCard := widget.NewCard("", "", container.NewBorder(headerRow, container.NewVBox(actions, a.statusBar), nil, nil, chatSplit))
	right := container.NewPadded(rightCard)

	split := container.NewHSplit(left, right)
	split.Offset = 0.32

	title := widget.NewLabel("LanTalk")
	title.TextStyle = fyne.TextStyle{Bold: true}
	a.topInfo = widget.NewLabel(fmt.Sprintf("%s  ·  %s", a.cfg.Name, shortID(a.cfg.ID)))
	settingBtn := widget.NewButtonWithIcon("", theme.SettingsIcon(), func() {
		a.openSettingsWindow()
	})
	topBar := container.NewMax(
		canvas.NewRectangle(color.NRGBA{R: 53, G: 146, B: 255, A: 255}),
		container.NewPadded(container.NewHBox(title, layout.NewSpacer(), a.topInfo, settingBtn)),
	)

	return container.NewBorder(topBar, nil, nil, nil, split)
}

func (a *appState) openSettingsWindow() {
	if a.settingsWin != nil {
		a.settingsWin.Show()
		a.settingsWin.RequestFocus()
		return
	}

	w := a.uiApp.NewWindow("个人设置")
	w.SetIcon(resourceAppIcon)
	w.Resize(fyne.NewSize(520, 420))

	avatarPreview := avatarImage(a.avatarAbsPath(), 132)
	a.settingsAvatar = avatarPreview

	nameInput := widget.NewEntry()
	nameInput.SetText(a.cfg.Name)
	idInput := widget.NewEntry()
	idInput.SetText(a.cfg.ID)
	idInput.Disable()

	pickAvatarBtn := widget.NewButton("选择头像", func() {
		dialog.ShowFileOpen(func(rc fyne.URIReadCloser, err error) {
			if err != nil || rc == nil {
				return
			}
			path := uriLocalPath(rc.URI())
			_ = rc.Close()
			if path == "" {
				return
			}
			a.applyAvatar(path, w)
		}, w)
	})
	clearAvatarBtn := widget.NewButton("移除头像", func() {
		a.cfg.Avatar = ""
		_ = a.saveConfig()
		a.refreshAvatarViews()
	})

	copyIDBtn := widget.NewButton("复制识别码", func() {
		w.Clipboard().SetContent(strings.TrimSpace(a.cfg.ID))
		a.pushStatus("已复制")
	})
	saveBtn := widget.NewButton("保存设置", func() {
		name := strings.TrimSpace(nameInput.Text)
		if name == "" {
			name = "LanUser"
		}
		a.cfg.Name = name
		if err := a.saveConfig(); err != nil {
			dialog.ShowError(err, w)
			return
		}
		if a.listenPort > 0 {
			a.broadcastHello(a.listenPort)
		}
		a.refreshContacts()
		a.refreshAvatarViews()
		if a.topInfo != nil {
			a.topInfo.SetText(fmt.Sprintf("%s  ·  %s", a.cfg.Name, shortID(a.cfg.ID)))
		}
		a.pushStatus("已保存")
	})

	avatarRow := container.NewHBox(
		avatarPreview,
		container.NewVBox(pickAvatarBtn, clearAvatarBtn),
	)
	infoForm := widget.NewForm(
		widget.NewFormItem("昵称", nameInput),
		widget.NewFormItem("识别码", idInput),
	)
	w.SetContent(container.NewPadded(widget.NewCard("", "", container.NewVBox(avatarRow, infoForm, container.NewHBox(copyIDBtn, layout.NewSpacer(), saveBtn)))))
	w.SetOnClosed(func() {
		a.settingsAvatar = nil
		a.settingsWin = nil
	})
	a.settingsWin = w
	w.Show()
}

func (a *appState) applyAvatar(srcPath string, owner fyne.Window) {
	if owner == nil {
		owner = a.win
	}
	ext := strings.ToLower(filepath.Ext(srcPath))
	if ext == "" {
		ext = ".png"
	}
	dst := filepath.Join(a.profileDir, "avatar"+ext)
	if err := copyFile(srcPath, dst); err != nil {
		dialog.ShowError(err, owner)
		return
	}
	a.cfg.Avatar = filepath.ToSlash(filepath.Join("data", "profile", filepath.Base(dst)))
	_ = a.saveConfig()
	a.refreshAvatarViews()
}

func (a *appState) avatarAbsPath() string {
	if strings.TrimSpace(a.cfg.Avatar) == "" {
		return ""
	}
	p := a.cfg.Avatar
	if filepath.IsAbs(p) {
		return p
	}
	return filepath.Join(a.baseDir, filepath.FromSlash(p))
}

func (a *appState) refreshAvatarViews() {
	path := a.avatarAbsPath()
	a.safeUI(func() {
		if a.settingsAvatar != nil {
			setAvatarImage(a.settingsAvatar, path)
		}
		if a.headerAvatar != nil {
			setAvatarImage(a.headerAvatar, path)
		}
		if a.chatHeader != nil {
			a.chatHeader.SetText("请选择联系人")
		}
	})
}

func (a *appState) openRemarkDialog() {
	key, p, ok := a.getActivePeer()
	if !ok {
		return
	}
	entry := widget.NewEntry()
	entry.SetText(a.getRemark(p.ID))
	content := container.NewVBox(widget.NewLabel("好友备注"), entry)
	dlg := dialog.NewCustomConfirm("设置备注", "保存", "取消", content, func(confirm bool) {
		if !confirm {
			return
		}
		a.setRemark(p.ID, entry.Text)
		a.refreshContacts()
		a.refreshHeaderByKey(key)
	}, a.win)
	dlg.Resize(fyne.NewSize(360, 160))
	dlg.Show()
}

func (a *appState) openEmojiPicker() {
	emojis := []string{
		"😀", "😁", "😂", "🤣", "😊", "😍", "😘", "😎",
		"🤔", "😴", "😭", "😡", "😱", "🥳", "🤝", "👍",
		"👏", "🙏", "💯", "🔥", "❤️", "💙", "🎉", "🌟",
	}
	grid := container.NewGridWithColumns(6)
	for _, e := range emojis {
		emoji := e
		btn := widget.NewButton(emoji, func() {
			a.insertToInput(emoji)
		})
		grid.Add(btn)
	}
	dlg := dialog.NewCustom("选择表情", "关闭", container.NewPadded(grid), a.win)
	dlg.Resize(fyne.NewSize(420, 260))
	dlg.Show()
}

func (a *appState) insertToInput(s string) {
	if a.inputBox == nil {
		return
	}
	a.inputBox.SetText(a.inputBox.Text + s)
}

func (a *appState) getRemark(userID string) string {
	a.mu.Lock()
	defer a.mu.Unlock()
	return strings.TrimSpace(a.cfg.Remarks[userID])
}

func (a *appState) setRemark(userID, remark string) {
	a.mu.Lock()
	if a.cfg.Remarks == nil {
		a.cfg.Remarks = map[string]string{}
	}
	remark = strings.TrimSpace(remark)
	if remark == "" {
		delete(a.cfg.Remarks, userID)
	} else {
		a.cfg.Remarks[userID] = remark
	}
	a.mu.Unlock()
	_ = a.saveConfig()
}

func (a *appState) bumpUnread(peerKey string) {
	a.mu.Lock()
	if a.activeKey == peerKey {
		a.mu.Unlock()
		return
	}
	if a.unread == nil {
		a.unread = map[string]int{}
	}
	a.unread[peerKey]++
	a.mu.Unlock()
	a.refreshContacts()
}

func (a *appState) clearUnread(peerKey string) {
	a.mu.Lock()
	_, ok := a.unread[peerKey]
	if ok {
		delete(a.unread, peerKey)
	}
	a.mu.Unlock()
	if ok {
		a.refreshContacts()
	}
}

func (a *appState) displayNameForPeer(p *peer) string {
	if p == nil {
		return ""
	}
	remark := a.getRemark(p.ID)
	if remark != "" {
		return remark
	}
	if strings.TrimSpace(p.Name) != "" {
		return p.Name
	}
	return p.ID
}

func (a *appState) refreshHeaderByKey(key string) {
	a.mu.Lock()
	p := a.peers[key]
	a.mu.Unlock()
	if p == nil {
		return
	}
	a.safeUI(func() {
		a.chatHeader.SetText(a.displayNameForPeer(p))
		a.chatSubHeader.SetText(fmt.Sprintf("%s · %s", p.Addr, shortID(p.ID)))
	})
}

func (a *appState) startTCPServer() (int, error) {
	listenAt := fmt.Sprintf("0.0.0.0:%d", defaultChatPort)
	ln, err := net.Listen("tcp4", listenAt)
	if err != nil {
		ln, err = net.Listen("tcp4", "0.0.0.0:0")
		if err != nil {
			return 0, err
		}
	}
	a.tcpListener = ln
	go func() {
		for {
			conn, err := ln.Accept()
			if err != nil {
				return
			}
			go a.handleTCPConn(conn)
		}
	}()
	return ln.Addr().(*net.TCPAddr).Port, nil
}

func (a *appState) handleTCPConn(conn net.Conn) {
	defer conn.Close()
	reader := bufio.NewReader(conn)
	env, err := readEnvelope(reader)
	if err != nil {
		return
	}
	host, _, _ := net.SplitHostPort(conn.RemoteAddr().String())

	switch env.Type {
	case "probe":
		a.handleProbe(conn, host, env)
	case "chat":
		a.handleChat(host, env)
	case "file_stream":
		a.handleFileStream(reader, host, env)
	}
}

func (a *appState) handleProbe(conn net.Conn, host string, env chatEnvelope) {
	if env.FromID != "" && env.FromPubKey != "" && env.FromInstance != a.instanceID {
		a.upsertPeer(peer{Key: makePeerKey(env.FromID, env.FromInstance), ID: env.FromID, InstanceID: env.FromInstance, Name: env.FromName, Addr: host, Port: env.Port, PublicKey: env.FromPubKey, LastSeen: time.Now()})
	}
	ack := chatEnvelope{Type: "probe_ack", FromID: a.cfg.ID, FromInstance: a.instanceID, FromName: a.cfg.Name, FromPubKey: a.cfg.PublicKey, Port: a.listenPort}
	_ = writeEnvelope(conn, ack)
}

func (a *appState) handleChat(host string, env chatEnvelope) {
	if env.FromPubKey == "" || env.FromInstance == a.instanceID {
		return
	}
	peerKey := makePeerKey(env.FromID, env.FromInstance)
	a.upsertPeer(peer{Key: peerKey, ID: env.FromID, InstanceID: env.FromInstance, Name: env.FromName, Addr: host, Port: env.Port, PublicKey: env.FromPubKey, LastSeen: time.Now()})

	payload, err := decrypt(env.FromPubKey, a.cfg.PrivateKey, env.Payload)
	if err != nil {
		return
	}
	if payload.SentAt == 0 {
		payload.SentAt = time.Now().Unix()
	}
	if payload.Kind != "" && payload.Kind != "text" {
		return
	}
	if strings.TrimSpace(payload.Text) == "" {
		return
	}
	line := historyLine{Direction: "in", From: a.displayNameForPeer(&peer{ID: env.FromID, Name: env.FromName}), Text: payload.Text, SentAt: payload.SentAt}
	a.appendHistory(peerKey, line)
	a.bumpUnread(peerKey)
	a.pushStatus("新消息 · " + line.From)
	a.refreshChatIfActive(peerKey)
}

func (a *appState) handleFileStream(reader *bufio.Reader, host string, env chatEnvelope) {
	if env.FromPubKey == "" || env.FromInstance == a.instanceID {
		return
	}
	peerKey := makePeerKey(env.FromID, env.FromInstance)
	a.upsertPeer(peer{Key: peerKey, ID: env.FromID, InstanceID: env.FromInstance, Name: env.FromName, Addr: host, Port: env.Port, PublicKey: env.FromPubKey, LastSeen: time.Now()})

	meta, err := decrypt(env.FromPubKey, a.cfg.PrivateKey, env.Payload)
	if err != nil {
		return
	}
	if (meta.Kind != "file_meta" && meta.Kind != "image_meta") || meta.FileName == "" || meta.FileSize < 0 || meta.FileSize > maxFileBytes {
		return
	}
	chunkSize := meta.ChunkSize
	if chunkSize <= 0 || chunkSize > 4<<20 {
		chunkSize = fileChunkBytes
	}

	salt, err := base64.StdEncoding.DecodeString(meta.Salt)
	if err != nil {
		return
	}
	key, err := deriveKey(a.cfg.PrivateKey, env.FromPubKey, salt)
	if err != nil {
		return
	}
	block, err := aes.NewCipher(key)
	if err != nil {
		return
	}
	gcm, err := cipher.NewGCM(block)
	if err != nil {
		return
	}

	name := sanitizeFilename(meta.FileName)
	if name == "" {
		name = "received.bin"
	}
	saveDir := a.downloadsDir
	label := "[文件]"
	if meta.Kind == "image_meta" {
		saveDir = filepath.Join(a.downloadsDir, "images")
		_ = os.MkdirAll(saveDir, 0o755)
		label = "[图片]"
	}
	finalPath := uniqueFilePath(saveDir, name)
	partPath := finalPath + ".part"
	out, err := os.Create(partPath)
	if err != nil {
		return
	}
	defer out.Close()

	var received int64
	for idx := uint64(0); ; idx++ {
		var clen uint32
		if err := binary.Read(reader, binary.BigEndian, &clen); err != nil {
			_ = os.Remove(partPath)
			return
		}
		if clen == 0 {
			break
		}
		if clen > uint32(chunkSize+gcm.Overhead()+64) {
			_ = os.Remove(partPath)
			return
		}
		buf := make([]byte, int(clen))
		if _, err := io.ReadFull(reader, buf); err != nil {
			_ = os.Remove(partPath)
			return
		}
		plain, err := gcm.Open(nil, chunkNonce(idx), buf, nil)
		if err != nil {
			_ = os.Remove(partPath)
			return
		}
		if received+int64(len(plain)) > meta.FileSize {
			_ = os.Remove(partPath)
			return
		}
		if _, err := out.Write(plain); err != nil {
			_ = os.Remove(partPath)
			return
		}
		received += int64(len(plain))
	}
	if received != meta.FileSize {
		_ = os.Remove(partPath)
		return
	}
	if err := out.Close(); err != nil {
		_ = os.Remove(partPath)
		return
	}
	if err := os.Rename(partPath, finalPath); err != nil {
		_ = os.Remove(partPath)
		return
	}
	rel, _ := filepath.Rel(a.baseDir, finalPath)
	displayFrom := a.displayNameForPeer(&peer{ID: env.FromID, Name: env.FromName})
	msg := fmt.Sprintf("%s %s (%s) -> %s", label, name, humanSize(received), rel)
	a.appendHistory(peerKey, historyLine{Direction: "in", From: displayFrom, Text: msg, SentAt: meta.SentAt})
	a.bumpUnread(peerKey)
	a.pushStatus("新消息 · " + displayFrom)
	a.refreshChatIfActive(peerKey)
}

func (a *appState) sendCurrentText() {
	text := strings.TrimSpace(a.inputBox.Text)
	if text == "" {
		return
	}
	peerKey, p, ok := a.getActivePeer()
	if !ok {
		return
	}
	payload := plainPayload{ID: randomID(8), Kind: "text", Text: text, SentAt: time.Now().Unix()}
	if err := a.sendPayload(peerKey, p, payload, text); err != nil {
		a.pushStatus("发送失败")
		return
	}
	a.inputBox.SetText("")
}

func (a *appState) sendCurrentFile() {
	a.sendLocalStream(false)
}

func (a *appState) sendCurrentImage() {
	a.sendLocalStream(true)
}

func (a *appState) sendLocalStream(imageOnly bool) {
	peerKey, p, ok := a.getActivePeer()
	if !ok {
		return
	}
	cb := func(rc fyne.URIReadCloser, err error) {
		if err != nil || rc == nil {
			return
		}
		path := uriLocalPath(rc.URI())
		_ = rc.Close()
		if path == "" {
			return
		}
		info, err := os.Stat(path)
		if err != nil {
			return
		}
		if info.Size() > maxFileBytes {
			dialog.ShowInformation("提示", "文件超过 100GB", a.win)
			return
		}
		name := sanitizeFilename(filepath.Base(path))
		metaKind := "file_meta"
		historyLabel := "[文件]"
		if imageOnly {
			if !isImagePath(path) {
				dialog.ShowInformation("提示", "请选择图片文件", a.win)
				return
			}
			metaKind = "image_meta"
			historyLabel = "[图片]"
		}
		go func() {
			if err := a.sendFileStream(peerKey, p, path, name, info.Size(), metaKind, historyLabel); err != nil {
				a.appendHistory(peerKey, historyLine{Direction: "out", From: "我", Text: historyLabel + " 发送失败", SentAt: time.Now().Unix()})
				a.refreshChatIfActive(peerKey)
				a.pushStatus("发送失败")
				return
			}
			a.pushStatus("发送完成")
		}()
	}
	if imageOnly {
		d := dialog.NewFileOpen(cb, a.win)
		d.SetFilter(storage.NewExtensionFileFilter([]string{".png", ".jpg", ".jpeg", ".gif", ".bmp", ".webp"}))
		d.Show()
		return
	}
	dialog.ShowFileOpen(cb, a.win)
}

func (a *appState) sendFileStream(peerKey string, p peer, path, name string, size int64, metaKind string, historyPrefix string) error {
	f, err := os.Open(path)
	if err != nil {
		return err
	}
	defer f.Close()

	salt := make([]byte, 16)
	if _, err := rand.Read(salt); err != nil {
		return err
	}
	key, err := deriveKey(a.cfg.PrivateKey, p.PublicKey, salt)
	if err != nil {
		return err
	}
	block, err := aes.NewCipher(key)
	if err != nil {
		return err
	}
	gcm, err := cipher.NewGCM(block)
	if err != nil {
		return err
	}

	meta := plainPayload{ID: randomID(8), Kind: metaKind, FileName: name, FileSize: size, ChunkSize: fileChunkBytes, Salt: base64.StdEncoding.EncodeToString(salt), SentAt: time.Now().Unix()}
	metaEnc, err := encrypt(p.PublicKey, a.cfg.PrivateKey, meta)
	if err != nil {
		return err
	}
	env := chatEnvelope{Type: "file_stream", FromID: a.cfg.ID, FromInstance: a.instanceID, FromName: a.cfg.Name, FromPubKey: a.cfg.PublicKey, Port: a.listenPort, Payload: metaEnc}

	conn, err := net.DialTimeout("tcp4", fmt.Sprintf("%s:%d", p.Addr, p.Port), 4*time.Second)
	if err != nil {
		return err
	}
	defer conn.Close()
	bw := bufio.NewWriterSize(conn, 128*1024)
	if err := writeEnvelope(bw, env); err != nil {
		return err
	}

	buf := make([]byte, fileChunkBytes)
	for idx := uint64(0); ; idx++ {
		n, rerr := f.Read(buf)
		if n > 0 {
			sealed := gcm.Seal(nil, chunkNonce(idx), buf[:n], nil)
			if err := binary.Write(bw, binary.BigEndian, uint32(len(sealed))); err != nil {
				return err
			}
			if _, err := bw.Write(sealed); err != nil {
				return err
			}
		}
		if errors.Is(rerr, io.EOF) {
			break
		}
		if rerr != nil {
			return rerr
		}
	}
	if err := binary.Write(bw, binary.BigEndian, uint32(0)); err != nil {
		return err
	}
	if err := bw.Flush(); err != nil {
		return err
	}

	history := fmt.Sprintf("%s %s (%s)", historyPrefix, name, humanSize(size))
	a.appendHistory(peerKey, historyLine{Direction: "out", From: "我", Text: history, SentAt: meta.SentAt})
	a.refreshChatIfActive(peerKey)
	return nil
}

func (a *appState) sendPayload(peerKey string, p peer, payload plainPayload, historyText string) error {
	payloadEnc, err := encrypt(p.PublicKey, a.cfg.PrivateKey, payload)
	if err != nil {
		return err
	}
	env := chatEnvelope{Type: "chat", FromID: a.cfg.ID, FromInstance: a.instanceID, FromName: a.cfg.Name, FromPubKey: a.cfg.PublicKey, Port: a.listenPort, Payload: payloadEnc}
	if err := sendEnvelope(p, env); err != nil {
		return err
	}
	a.appendHistory(peerKey, historyLine{Direction: "out", From: "我", Text: historyText, SentAt: payload.SentAt})
	a.refreshChatIfActive(peerKey)
	return nil
}

func (a *appState) startDiscovery(chatPort int) error {
	conn, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4zero, Port: discoveryPort})
	if err != nil {
		return err
	}
	_ = conn.SetReadBuffer(64 * 1024)
	a.udpConn = conn

	pc := ipv4.NewPacketConn(conn)
	ifaces, _ := net.Interfaces()
	for i := range ifaces {
		iface := ifaces[i]
		if iface.Flags&net.FlagUp == 0 {
			continue
		}
		_ = pc.JoinGroup(&iface, &net.UDPAddr{IP: net.ParseIP(discoveryHost)})
	}

	go func() {
		buf := make([]byte, 4096)
		for {
			n, addr, err := conn.ReadFromUDP(buf)
			if err != nil {
				return
			}
			var p discoveryPacket
			if err := json.Unmarshal(buf[:n], &p); err != nil {
				continue
			}
			if p.Type != "hello" || p.ID == "" || p.PublicKey == "" || p.InstanceID == a.instanceID {
				continue
			}
			a.upsertPeer(peer{Key: makePeerKey(p.ID, p.InstanceID), ID: p.ID, InstanceID: p.InstanceID, Name: p.Name, Addr: addr.IP.String(), Port: p.Port, PublicKey: p.PublicKey, LastSeen: time.Now()})
		}
	}()

	go func() {
		ticker := time.NewTicker(announceEvery)
		defer ticker.Stop()
		for {
			a.broadcastHello(chatPort)
			select {
			case <-ticker.C:
			case <-a.stopCh:
				return
			}
		}
	}()

	go func() {
		ticker := time.NewTicker(5 * time.Second)
		defer ticker.Stop()
		for {
			select {
			case <-ticker.C:
				changed := false
				now := time.Now()
				a.mu.Lock()
				for key, p := range a.peers {
					if now.Sub(p.LastSeen) > presenceTTL {
						delete(a.peers, key)
						changed = true
						if a.activeKey == key {
							a.activeKey = ""
						}
					}
				}
				a.mu.Unlock()
				if changed {
					a.refreshContacts()
				}
			case <-a.stopCh:
				return
			}
		}
	}()

	go func() {
		ticker := time.NewTicker(autoProbeEvery)
		defer ticker.Stop()
		for {
			go a.probeAutoTargets()
			select {
			case <-ticker.C:
			case <-a.stopCh:
				return
			}
		}
	}()

	go a.probeAutoTargets()
	return nil
}

func (a *appState) broadcastHello(chatPort int) {
	pkt := discoveryPacket{Type: "hello", ID: a.cfg.ID, InstanceID: a.instanceID, Name: a.cfg.Name, PublicKey: a.cfg.PublicKey, Port: chatPort, Time: time.Now().Unix()}
	b, _ := json.Marshal(pkt)
	targets := []net.IP{net.ParseIP(discoveryHost), net.IPv4bcast}
	targets = append(targets, localDirectedBroadcasts()...)
	for _, ip := range targets {
		if ip == nil {
			continue
		}
		conn, err := net.DialUDP("udp4", nil, &net.UDPAddr{IP: ip, Port: discoveryPort})
		if err != nil {
			continue
		}
		_, _ = conn.Write(b)
		_ = conn.Close()
	}
}

func (a *appState) probeAutoTargets() {
	targets := a.collectAutoProbeTargets()
	sem := make(chan struct{}, probeParallelism)
	var wg sync.WaitGroup
	for _, t := range targets {
		wg.Add(1)
		sem <- struct{}{}
		go func(target string) {
			defer wg.Done()
			_ = a.probeTarget(target)
			<-sem
		}(t)
	}
	wg.Wait()
}

func (a *appState) collectAutoProbeTargets() []string {
	set := map[string]struct{}{}
	a.mu.Lock()
	for _, p := range a.peers {
		if p.Addr == "" {
			continue
		}
		port := p.Port
		if port == 0 {
			port = defaultChatPort
		}
		set[fmt.Sprintf("%s:%d", p.Addr, port)] = struct{}{}
		set[fmt.Sprintf("%s:%d", p.Addr, defaultChatPort)] = struct{}{}
	}
	a.mu.Unlock()

	for _, ip := range localIPv4Addrs() {
		if !isPrivateIPv4(ip) {
			continue
		}
		for delta := -1; delta <= 1; delta++ {
			third := int(ip[2]) + delta
			if third < 0 || third > 255 {
				continue
			}
			for host := 1; host <= 254; host++ {
				if delta == 0 && host == int(ip[3]) {
					continue
				}
				set[fmt.Sprintf("%d.%d.%d.%d:%d", ip[0], ip[1], third, host, defaultChatPort)] = struct{}{}
			}
		}
	}

	all := make([]string, 0, len(set))
	for t := range set {
		all = append(all, t)
	}
	sort.Strings(all)
	if len(all) <= scanBatchSize {
		return all
	}

	a.mu.Lock()
	start := a.scanOffset % len(all)
	a.scanOffset = (a.scanOffset + scanBatchSize) % len(all)
	a.mu.Unlock()

	batch := make([]string, 0, scanBatchSize)
	for i := 0; i < scanBatchSize; i++ {
		batch = append(batch, all[(start+i)%len(all)])
	}
	return batch
}

func (a *appState) probeTarget(target string) error {
	conn, err := net.DialTimeout("tcp4", target, 350*time.Millisecond)
	if err != nil {
		return err
	}
	defer conn.Close()
	_ = conn.SetDeadline(time.Now().Add(900 * time.Millisecond))

	probe := chatEnvelope{Type: "probe", FromID: a.cfg.ID, FromInstance: a.instanceID, FromName: a.cfg.Name, FromPubKey: a.cfg.PublicKey, Port: a.listenPort}
	if err := writeEnvelope(conn, probe); err != nil {
		return err
	}
	ack, err := readEnvelope(bufio.NewReader(conn))
	if err != nil {
		return err
	}
	if ack.Type != "probe_ack" || ack.FromID == "" || ack.FromPubKey == "" || ack.FromInstance == a.instanceID {
		return errors.New("invalid ack")
	}
	host, _, _ := net.SplitHostPort(target)
	a.upsertPeer(peer{Key: makePeerKey(ack.FromID, ack.FromInstance), ID: ack.FromID, InstanceID: ack.FromInstance, Name: ack.FromName, Addr: host, Port: ack.Port, PublicKey: ack.FromPubKey, LastSeen: time.Now()})
	return nil
}

func (a *appState) upsertPeer(in peer) {
	if in.Key == "" {
		in.Key = makePeerKey(in.ID, in.InstanceID)
	}
	if in.Key == makePeerKey(a.cfg.ID, a.instanceID) || in.PublicKey == "" {
		return
	}
	if in.Name == "" {
		in.Name = in.ID
	}
	a.mu.Lock()
	exist := a.peers[in.Key]
	if exist != nil && exist.PublicKey != "" && exist.PublicKey != in.PublicKey {
		a.mu.Unlock()
		return
	}
	if exist == nil {
		cp := in
		a.peers[in.Key] = &cp
	} else {
		exist.Name = in.Name
		if in.Addr != "" {
			exist.Addr = in.Addr
		}
		if in.Port != 0 {
			exist.Port = in.Port
		}
		exist.LastSeen = in.LastSeen
	}
	a.mu.Unlock()
	a.refreshContacts()
}

func (a *appState) refreshContacts() {
	a.mu.Lock()
	pairs := make([]*peer, 0, len(a.peers))
	for _, p := range a.peers {
		pairs = append(pairs, p)
	}
	unread := make(map[string]int, len(a.unread))
	for k, v := range a.unread {
		unread[k] = v
	}
	filter := strings.ToLower(strings.TrimSpace(a.peerFilter))
	a.mu.Unlock()

	sort.Slice(pairs, func(i, j int) bool {
		di := strings.ToLower(a.displayNameForPeer(pairs[i]))
		dj := strings.ToLower(a.displayNameForPeer(pairs[j]))
		if di == dj {
			return pairs[i].Addr < pairs[j].Addr
		}
		return di < dj
	})

	rows := make([]peerRow, 0, len(pairs))
	for _, p := range pairs {
		title := a.displayNameForPeer(p)
		sub := fmt.Sprintf("%s · %s", p.Addr, shortID(p.ID))
		unreadCount := unread[p.Key]
		if unreadCount > 0 {
			title = fmt.Sprintf("%s (%d)", title, unreadCount)
			sub = "有新消息 · " + sub
		}
		probe := strings.ToLower(title + " " + sub)
		if filter != "" && !strings.Contains(probe, filter) {
			continue
		}
		rows = append(rows, peerRow{Key: p.Key, Title: title, Subtitle: sub, Unread: unreadCount})
	}

	a.mu.Lock()
	a.peerRows = rows
	a.mu.Unlock()
	a.safeUI(func() { a.contactBox.Refresh() })
}

func (a *appState) selectPeer(index int) {
	a.mu.Lock()
	if index < 0 || index >= len(a.peerRows) {
		a.mu.Unlock()
		return
	}
	key := a.peerRows[index].Key
	p := a.peers[key]
	a.activeKey = key
	a.mu.Unlock()
	if p == nil {
		return
	}
	a.safeUI(func() {
		a.chatHeader.SetText(a.displayNameForPeer(p))
		a.chatSubHeader.SetText(fmt.Sprintf("%s · %s", p.Addr, shortID(p.ID)))
	})
	a.clearUnread(key)
	a.renderHistory(key)
}

func (a *appState) getActivePeer() (string, peer, bool) {
	a.mu.Lock()
	defer a.mu.Unlock()
	if a.activeKey == "" {
		return "", peer{}, false
	}
	p := a.peers[a.activeKey]
	if p == nil {
		return "", peer{}, false
	}
	return a.activeKey, *p, true
}

func (a *appState) renderHistory(peerKey string) {
	history, _ := a.loadHistory(peerKey)
	items := make([]fyne.CanvasObject, 0, len(history))
	for _, m := range history {
		items = append(items, renderBubbleMessage(m))
	}
	a.safeUI(func() {
		if a.chatStream == nil {
			return
		}
		a.chatStream.Objects = items
		a.chatStream.Refresh()
		if a.chatScroll != nil {
			a.chatScroll.ScrollToBottom()
		}
	})
}

func renderBubbleMessage(m historyLine) fyne.CanvasObject {
	outgoing := m.Direction == "out"
	meta := time.Unix(m.SentAt, 0).Format("15:04")
	if outgoing {
		meta = "我  " + meta
	} else if strings.TrimSpace(m.From) != "" {
		meta = m.From + "  " + meta
	}

	metaLabel := widget.NewLabel(meta)
	bodyLabel := widget.NewLabel(m.Text)
	bodyLabel.Wrapping = fyne.TextWrapWord

	bgColor := color.NRGBA{R: 255, G: 255, B: 255, A: 255}
	if outgoing {
		bgColor = color.NRGBA{R: 217, G: 235, B: 255, A: 255}
	}
	bubble := container.NewMax(
		canvas.NewRectangle(bgColor),
		container.NewPadded(bodyLabel),
	)

	var metaRow *fyne.Container
	var bodyRow *fyne.Container
	if outgoing {
		metaRow = container.NewHBox(layout.NewSpacer(), metaLabel)
		bodyRow = container.NewHBox(layout.NewSpacer(), container.NewPadded(bubble))
	} else {
		metaRow = container.NewHBox(metaLabel, layout.NewSpacer())
		bodyRow = container.NewHBox(container.NewPadded(bubble), layout.NewSpacer())
	}
	return container.NewVBox(metaRow, bodyRow)
}

func (a *appState) refreshChatIfActive(peerKey string) {
	a.mu.Lock()
	active := a.activeKey
	a.mu.Unlock()
	if active == peerKey {
		a.renderHistory(peerKey)
	}
}

func (a *appState) appendHistory(peerKey string, line historyLine) {
	a.mu.Lock()
	defer a.mu.Unlock()
	path := a.historyPath(peerKey)
	list := []historyLine{}
	if b, err := os.ReadFile(path); err == nil {
		_ = json.Unmarshal(b, &list)
	}
	list = append(list, line)
	b, _ := json.MarshalIndent(list, "", "  ")
	_ = os.WriteFile(path, b, 0o644)
}

func (a *appState) loadHistory(peerKey string) ([]historyLine, error) {
	path := a.historyPath(peerKey)
	b, err := os.ReadFile(path)
	if err != nil {
		if errors.Is(err, os.ErrNotExist) {
			return []historyLine{}, nil
		}
		return nil, err
	}
	list := []historyLine{}
	if err := json.Unmarshal(b, &list); err != nil {
		return nil, err
	}
	return list, nil
}

func (a *appState) historyPath(peerKey string) string {
	return filepath.Join(a.chatsDir, sanitizeFilename(peerKey)+".json")
}

func (a *appState) pushStatus(text string) {
	a.safeUI(func() { a.statusBar.SetText(text) })
}

func (a *appState) safeUI(fn func()) {
	if a.uiApp == nil {
		return
	}
	fyne.Do(fn)
}

func (a *appState) shutdown() {
	a.stopOnce.Do(func() {
		close(a.stopCh)
		if a.udpConn != nil {
			_ = a.udpConn.Close()
		}
		if a.tcpListener != nil {
			_ = a.tcpListener.Close()
		}
	})
}

func sendEnvelope(p peer, env chatEnvelope) error {
	if p.Port == 0 || p.Addr == "" {
		return errors.New("unreachable")
	}
	conn, err := net.DialTimeout("tcp4", fmt.Sprintf("%s:%d", p.Addr, p.Port), 2*time.Second)
	if err != nil {
		return err
	}
	defer conn.Close()
	_ = conn.SetDeadline(time.Now().Add(4 * time.Second))
	return writeEnvelope(conn, env)
}

func writeEnvelope(w io.Writer, env chatEnvelope) error {
	b, err := json.Marshal(env)
	if err != nil {
		return err
	}
	if len(b) > maxEnvelopeBytes {
		return errors.New("envelope too large")
	}
	b = append(b, '\n')
	_, err = w.Write(b)
	return err
}

func readEnvelope(r *bufio.Reader) (chatEnvelope, error) {
	line, err := r.ReadBytes('\n')
	if err != nil {
		return chatEnvelope{}, err
	}
	if len(line) > maxEnvelopeBytes {
		return chatEnvelope{}, errors.New("envelope too large")
	}
	line = bytes.TrimSpace(line)
	if len(line) == 0 {
		return chatEnvelope{}, errors.New("empty envelope")
	}
	var env chatEnvelope
	if err := json.Unmarshal(line, &env); err != nil {
		return chatEnvelope{}, err
	}
	return env, nil
}

func generateKeyPair() (pubB64, privB64 string, err error) {
	priv := make([]byte, 32)
	if _, err = rand.Read(priv); err != nil {
		return "", "", err
	}
	pub, err := curve25519.X25519(priv, curve25519.Basepoint)
	if err != nil {
		return "", "", err
	}
	return base64.StdEncoding.EncodeToString(pub), base64.StdEncoding.EncodeToString(priv), nil
}

func encrypt(peerPubB64, selfPrivB64 string, payload plainPayload) (encryptedMessage, error) {
	plainBytes, err := json.Marshal(payload)
	if err != nil {
		return encryptedMessage{}, err
	}
	salt := make([]byte, 16)
	nonce := make([]byte, 12)
	if _, err := rand.Read(salt); err != nil {
		return encryptedMessage{}, err
	}
	if _, err := rand.Read(nonce); err != nil {
		return encryptedMessage{}, err
	}
	key, err := deriveKey(selfPrivB64, peerPubB64, salt)
	if err != nil {
		return encryptedMessage{}, err
	}
	block, err := aes.NewCipher(key)
	if err != nil {
		return encryptedMessage{}, err
	}
	gcm, err := cipher.NewGCM(block)
	if err != nil {
		return encryptedMessage{}, err
	}
	cipherText := gcm.Seal(nil, nonce, plainBytes, nil)
	return encryptedMessage{Salt: base64.StdEncoding.EncodeToString(salt), Nonce: base64.StdEncoding.EncodeToString(nonce), Data: base64.StdEncoding.EncodeToString(cipherText)}, nil
}

func decrypt(senderPubB64, selfPrivB64 string, c encryptedMessage) (plainPayload, error) {
	salt, err := base64.StdEncoding.DecodeString(c.Salt)
	if err != nil {
		return plainPayload{}, err
	}
	nonce, err := base64.StdEncoding.DecodeString(c.Nonce)
	if err != nil {
		return plainPayload{}, err
	}
	cipherText, err := base64.StdEncoding.DecodeString(c.Data)
	if err != nil {
		return plainPayload{}, err
	}
	key, err := deriveKey(selfPrivB64, senderPubB64, salt)
	if err != nil {
		return plainPayload{}, err
	}
	block, err := aes.NewCipher(key)
	if err != nil {
		return plainPayload{}, err
	}
	gcm, err := cipher.NewGCM(block)
	if err != nil {
		return plainPayload{}, err
	}
	plainBytes, err := gcm.Open(nil, nonce, cipherText, nil)
	if err != nil {
		return plainPayload{}, err
	}
	var p plainPayload
	if err := json.Unmarshal(plainBytes, &p); err != nil {
		return plainPayload{}, err
	}
	return p, nil
}

func deriveKey(privB64, pubB64 string, salt []byte) ([]byte, error) {
	priv, err := base64.StdEncoding.DecodeString(privB64)
	if err != nil {
		return nil, err
	}
	pub, err := base64.StdEncoding.DecodeString(pubB64)
	if err != nil {
		return nil, err
	}
	shared, err := curve25519.X25519(priv, pub)
	if err != nil {
		return nil, err
	}
	kdf := hkdf.New(sha256.New, shared, salt, []byte("LanTalk-E2E-v1"))
	key := make([]byte, 32)
	if _, err := io.ReadFull(kdf, key); err != nil {
		return nil, err
	}
	return key, nil
}

func avatarImage(path string, size float32) *canvas.Image {
	img := canvas.NewImageFromResource(resourceDefaultAvatar)
	img.FillMode = canvas.ImageFillContain
	img.SetMinSize(fyne.NewSize(size, size))
	setAvatarImage(img, path)
	return img
}

func setAvatarImage(img *canvas.Image, path string) {
	if img == nil {
		return
	}
	if path != "" {
		if _, err := os.Stat(path); err == nil {
			img.File = path
			img.Resource = nil
			img.Refresh()
			return
		}
	}
	img.File = ""
	img.Resource = resourceDefaultAvatar
	img.Refresh()
}

func copyFile(src, dst string) error {
	in, err := os.Open(src)
	if err != nil {
		return err
	}
	defer in.Close()
	if err := os.MkdirAll(filepath.Dir(dst), 0o755); err != nil {
		return err
	}
	out, err := os.Create(dst)
	if err != nil {
		return err
	}
	defer out.Close()
	if _, err := io.Copy(out, in); err != nil {
		return err
	}
	return out.Sync()
}

func newUserCode() string {
	return strings.ToUpper(randomID(16))
}

func randomID(n int) string {
	b := make([]byte, n)
	if _, err := rand.Read(b); err != nil {
		return fmt.Sprintf("fallback-%d", time.Now().UnixNano())
	}
	return hex.EncodeToString(b)
}

func makePeerKey(id, instance string) string {
	if instance == "" {
		return id
	}
	return id + "@" + instance
}

func chunkNonce(idx uint64) []byte {
	nonce := make([]byte, 12)
	binary.BigEndian.PutUint64(nonce[4:], idx)
	return nonce
}

func uriLocalPath(u fyne.URI) string {
	if u == nil {
		return ""
	}
	p := u.Path()
	if runtime.GOOS == "windows" && strings.HasPrefix(p, "/") && len(p) > 2 && p[2] == ':' {
		p = p[1:]
	}
	return filepath.FromSlash(p)
}

func sanitizeFilename(s string) string {
	if s == "" {
		return ""
	}
	s = filepath.Base(s)
	var b strings.Builder
	for _, r := range s {
		switch {
		case r >= 'a' && r <= 'z':
			b.WriteRune(r)
		case r >= 'A' && r <= 'Z':
			b.WriteRune(r)
		case r >= '0' && r <= '9':
			b.WriteRune(r)
		case r == '.', r == '-', r == '_', r == '@':
			b.WriteRune(r)
		default:
			b.WriteRune('_')
		}
	}
	out := strings.Trim(b.String(), "._")
	if out == "" {
		return "file"
	}
	return out
}

func isImagePath(path string) bool {
	ext := strings.ToLower(filepath.Ext(path))
	switch ext {
	case ".png", ".jpg", ".jpeg", ".gif", ".bmp", ".webp":
		return true
	default:
		return false
	}
}

func uniqueFilePath(dir, name string) string {
	base := strings.TrimSuffix(name, filepath.Ext(name))
	ext := filepath.Ext(name)
	candidate := filepath.Join(dir, name)
	if _, err := os.Stat(candidate); errors.Is(err, os.ErrNotExist) {
		return candidate
	}
	for i := 1; i < 1000; i++ {
		candidate = filepath.Join(dir, fmt.Sprintf("%s_%d%s", base, i, ext))
		if _, err := os.Stat(candidate); errors.Is(err, os.ErrNotExist) {
			return candidate
		}
	}
	return filepath.Join(dir, fmt.Sprintf("%s_%d%s", base, time.Now().Unix(), ext))
}

func humanSize(n int64) string {
	if n < 1024 {
		return fmt.Sprintf("%d B", n)
	}
	units := []string{"KB", "MB", "GB", "TB"}
	f := float64(n)
	idx := -1
	for f >= 1024 && idx < len(units)-1 {
		f /= 1024
		idx++
	}
	if idx < 0 {
		return fmt.Sprintf("%d B", n)
	}
	return fmt.Sprintf("%.1f %s", f, units[idx])
}

func shortID(s string) string {
	if len(s) <= 6 {
		return s
	}
	return s[:6]
}

func localIPv4Addrs() []net.IP {
	ifaces, _ := net.Interfaces()
	out := []net.IP{}
	for i := range ifaces {
		iface := ifaces[i]
		if iface.Flags&net.FlagUp == 0 {
			continue
		}
		addrs, _ := iface.Addrs()
		for _, a := range addrs {
			ipNet, ok := a.(*net.IPNet)
			if !ok {
				continue
			}
			ip := ipNet.IP.To4()
			if ip == nil || ip.IsLoopback() {
				continue
			}
			cp := make(net.IP, 4)
			copy(cp, ip)
			out = append(out, cp)
		}
	}
	return out
}

func localDirectedBroadcasts() []net.IP {
	ifaces, _ := net.Interfaces()
	set := map[string]net.IP{}
	for i := range ifaces {
		iface := ifaces[i]
		if iface.Flags&net.FlagUp == 0 {
			continue
		}
		addrs, _ := iface.Addrs()
		for _, a := range addrs {
			ipNet, ok := a.(*net.IPNet)
			if !ok {
				continue
			}
			ip := ipNet.IP.To4()
			mask := ipNet.Mask
			if ip == nil || len(mask) != 4 {
				continue
			}
			b := make(net.IP, 4)
			for j := 0; j < 4; j++ {
				b[j] = ip[j] | ^mask[j]
			}
			set[b.String()] = b
		}
	}
	out := make([]net.IP, 0, len(set))
	for _, ip := range set {
		out = append(out, ip)
	}
	return out
}

func isPrivateIPv4(ip net.IP) bool {
	if ip == nil || len(ip) != 4 {
		return false
	}
	if ip[0] == 10 {
		return true
	}
	if ip[0] == 172 && ip[1] >= 16 && ip[1] <= 31 {
		return true
	}
	if ip[0] == 192 && ip[1] == 168 {
		return true
	}
	return false
}
