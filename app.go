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
	Key        string
	Name       string
	Preview    string
	Online     bool
	Unread     int
	LastSentAt int64
}

type knownPeer struct {
	Key        string `json:"key"`
	ID         string `json:"id"`
	InstanceID string `json:"instanceId,omitempty"`
	Name       string `json:"name,omitempty"`
	Addr       string `json:"addr,omitempty"`
	LastText   string `json:"lastText,omitempty"`
	LastSentAt int64  `json:"lastSentAt,omitempty"`
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

type appTheme struct{}

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

type emojiTile struct {
	widget.BaseWidget
	emoji string
	onTap func(string)
}

func newEmojiTile(emoji string, onTap func(string)) *emojiTile {
	tile := &emojiTile{emoji: emoji, onTap: onTap}
	tile.ExtendBaseWidget(tile)
	return tile
}

func (t *emojiTile) Tapped(*fyne.PointEvent) {
	if t.onTap != nil {
		t.onTap(t.emoji)
	}
}

func (t *emojiTile) TappedSecondary(*fyne.PointEvent) {}

func (t *emojiTile) CreateRenderer() fyne.WidgetRenderer {
	bg := canvas.NewRectangle(color.Transparent)
	bg.CornerRadius = 6
	txt := canvas.NewText(t.emoji, color.NRGBA{R: 40, G: 45, B: 55, A: 255})
	txt.Alignment = fyne.TextAlignCenter
	txt.TextSize = 24
	content := container.NewMax(bg, container.NewCenter(txt))
	return &emojiTileRenderer{tile: t, bg: bg, txt: txt, content: content}
}

type emojiTileRenderer struct {
	tile    *emojiTile
	bg      *canvas.Rectangle
	txt     *canvas.Text
	content *fyne.Container
}

func (r *emojiTileRenderer) Layout(size fyne.Size) {
	r.content.Resize(size)
}

func (r *emojiTileRenderer) MinSize() fyne.Size {
	return fyne.NewSize(42, 42)
}

func (r *emojiTileRenderer) Refresh() {
	r.bg.FillColor = color.Transparent
	r.txt.Text = r.tile.emoji
	r.bg.Refresh()
	r.txt.Refresh()
}

func (r *emojiTileRenderer) BackgroundColor() color.Color {
	return color.Transparent
}

func (r *emojiTileRenderer) Objects() []fyne.CanvasObject {
	return []fyne.CanvasObject{r.content}
}

func (r *emojiTileRenderer) Destroy() {}

func (appTheme) Color(name fyne.ThemeColorName, variant fyne.ThemeVariant) color.Color {
	switch name {
	case theme.ColorNamePrimary:
		return color.NRGBA{R: 107, G: 117, B: 126, A: 255}
	case theme.ColorNameBackground:
		return color.NRGBA{R: 244, G: 246, B: 248, A: 255}
	case theme.ColorNameInputBackground:
		return color.NRGBA{R: 255, G: 255, B: 255, A: 255}
	case theme.ColorNameShadow:
		return color.NRGBA{R: 220, G: 224, B: 229, A: 255}
	case theme.ColorNameHover:
		return color.NRGBA{R: 232, G: 236, B: 241, A: 255}
	default:
		return theme.DefaultTheme().Color(name, variant)
	}
}
func (appTheme) Font(style fyne.TextStyle) fyne.Resource    { return theme.DefaultTheme().Font(style) }
func (appTheme) Icon(name fyne.ThemeIconName) fyne.Resource { return theme.DefaultTheme().Icon(name) }
func (appTheme) Size(name fyne.ThemeSizeName) float32 {
	if name == theme.SizeNamePadding {
		return 2
	}
	return theme.DefaultTheme().Size(name)
}

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
	knownPeers     map[string]knownPeer
	unread         map[string]int
	peerRows       []peerRow
	activeKey      string
	scanOffset     int
	tcpListener    net.Listener
	udpConn        *net.UDPConn
	stopOnce       sync.Once
	stopCh         chan struct{}
	uiApp          fyne.App
	win            fyne.Window
	contactBox     *widget.List
	rightHost      *fyne.Container
	chatPanel      fyne.CanvasObject
	selfAvatar     *canvas.Image
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
		knownPeers:   map[string]knownPeer{},
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
	if err := a.saveConfig(); err != nil {
		return err
	}
	_ = a.loadKnownPeers()
	_ = a.seedKnownPeersFromHistory()
	return nil
}

func (a *appState) saveConfig() error {
	b, err := json.MarshalIndent(a.cfg, "", "  ")
	if err != nil {
		return err
	}
	return os.WriteFile(filepath.Join(a.dataDir, "config.json"), b, 0o644)
}

func (a *appState) knownPeersPath() string {
	return filepath.Join(a.dataDir, "known_peers.json")
}

func (a *appState) loadKnownPeers() error {
	b, err := os.ReadFile(a.knownPeersPath())
	if err != nil {
		if errors.Is(err, os.ErrNotExist) {
			return nil
		}
		return err
	}
	data := map[string]knownPeer{}
	if err := json.Unmarshal(b, &data); err != nil {
		return err
	}
	if data == nil {
		data = map[string]knownPeer{}
	}
	a.mu.Lock()
	a.knownPeers = data
	a.mu.Unlock()
	return nil
}

func (a *appState) saveKnownPeers() error {
	a.mu.Lock()
	data := make(map[string]knownPeer, len(a.knownPeers))
	for k, v := range a.knownPeers {
		data[k] = v
	}
	a.mu.Unlock()
	b, err := json.MarshalIndent(data, "", "  ")
	if err != nil {
		return err
	}
	return os.WriteFile(a.knownPeersPath(), b, 0o644)
}

func (a *appState) seedKnownPeersFromHistory() error {
	entries, err := os.ReadDir(a.chatsDir)
	if err != nil {
		return err
	}
	changed := false
	for _, e := range entries {
		if e.IsDir() || !strings.HasSuffix(strings.ToLower(e.Name()), ".json") {
			continue
		}
		key := strings.TrimSuffix(e.Name(), filepath.Ext(e.Name()))
		if key == "" {
			continue
		}
		lastText := ""
		lastSentAt := int64(0)
		history, err := a.loadHistory(key)
		if err == nil && len(history) > 0 {
			last := history[len(history)-1]
			lastText = last.Text
			lastSentAt = last.SentAt
		}
		a.mu.Lock()
		if a.upsertKnownPeerLocked(key, knownPeer{LastText: lastText, LastSentAt: lastSentAt}) {
			changed = true
		}
		a.mu.Unlock()
	}
	if changed {
		return a.saveKnownPeers()
	}
	return nil
}

func (a *appState) upsertKnownPeerLocked(key string, in knownPeer) bool {
	if a.knownPeers == nil {
		a.knownPeers = map[string]knownPeer{}
	}
	cur := a.knownPeers[key]
	if cur.Key == "" {
		cur.Key = key
	}
	if cur.ID == "" || cur.InstanceID == "" {
		id, instance := splitPeerKey(key)
		if cur.ID == "" {
			cur.ID = id
		}
		if cur.InstanceID == "" {
			cur.InstanceID = instance
		}
	}
	changed := false
	if strings.TrimSpace(in.ID) != "" && cur.ID != strings.TrimSpace(in.ID) {
		cur.ID = strings.TrimSpace(in.ID)
		changed = true
	}
	if strings.TrimSpace(in.InstanceID) != "" && cur.InstanceID != strings.TrimSpace(in.InstanceID) {
		cur.InstanceID = strings.TrimSpace(in.InstanceID)
		changed = true
	}
	if strings.TrimSpace(in.Name) != "" && cur.Name != strings.TrimSpace(in.Name) {
		cur.Name = strings.TrimSpace(in.Name)
		changed = true
	}
	if strings.TrimSpace(in.Addr) != "" && cur.Addr != strings.TrimSpace(in.Addr) {
		cur.Addr = strings.TrimSpace(in.Addr)
		changed = true
	}
	if in.LastSentAt > 0 && in.LastSentAt >= cur.LastSentAt {
		text := strings.TrimSpace(in.LastText)
		if cur.LastSentAt != in.LastSentAt || (text != "" && cur.LastText != text) {
			cur.LastSentAt = in.LastSentAt
			if text != "" {
				cur.LastText = text
			}
			changed = true
		}
	}
	if _, ok := a.knownPeers[key]; !ok {
		changed = true
	}
	a.knownPeers[key] = cur
	return changed
}

func (a *appState) buildUI() error {
	a.uiApp = app.NewWithID("lantalk")
	a.uiApp.SetIcon(resourceAppIcon)
	a.uiApp.Settings().SetTheme(appTheme{})
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
	a.contactBox = widget.NewList(
		func() int {
			a.mu.Lock()
			defer a.mu.Unlock()
			return len(a.peerRows)
		},
		func() fyne.CanvasObject {
			avatar := avatarImage("", 32)

			name := widget.NewLabel("")
			name.TextStyle = fyne.TextStyle{Bold: true}
			dotBase := canvas.NewRectangle(color.Transparent)
			dotBase.SetMinSize(fyne.NewSize(8, 8))
			statusDot := canvas.NewCircle(color.NRGBA{R: 153, G: 160, B: 170, A: 255})
			statusWrap := container.NewMax(dotBase, statusDot)
			top := container.NewHBox(name, spacerBox(6), statusWrap, layout.NewSpacer())

			preview := widget.NewLabel("")
			preview.Wrapping = fyne.TextWrapOff
			preview.Importance = widget.LowImportance
			textCol := container.NewVBox(top, preview)
			return container.NewHBox(spacerBox(8), avatar, spacerBox(8), textCol, spacerBox(8))
		},
		func(id widget.ListItemID, obj fyne.CanvasObject) {
			a.mu.Lock()
			row := peerRow{}
			if id >= 0 && id < len(a.peerRows) {
				row = a.peerRows[id]
			}
			a.mu.Unlock()
			c := obj.(*fyne.Container)
			textCol := c.Objects[3].(*fyne.Container)
			top := textCol.Objects[0].(*fyne.Container)
			nameLabel := top.Objects[0].(*widget.Label)
			statusWrap := top.Objects[2].(*fyne.Container)
			statusDot := statusWrap.Objects[1].(*canvas.Circle)
			previewLabel := textCol.Objects[1].(*widget.Label)

			statusDot.FillColor = color.NRGBA{R: 153, G: 160, B: 170, A: 255}
			if row.Online {
				statusDot.FillColor = color.NRGBA{R: 48, G: 191, B: 88, A: 255}
			}
			statusDot.Refresh()
			nameLabel.SetText(row.Name)
			previewLabel.SetText(row.Preview)
		},
	)
	a.contactBox.OnSelected = func(id widget.ListItemID) { a.selectPeer(int(id)) }

	leftGray := color.NRGBA{R: 236, G: 239, B: 243, A: 255}
	rightGray := color.NRGBA{R: 244, G: 246, B: 248, A: 255}
	lineGray := color.NRGBA{R: 220, G: 224, B: 229, A: 255}

	a.selfAvatar = avatarImage(a.avatarAbsPath(), 40)
	settingBtn := widget.NewButtonWithIcon("", theme.SettingsIcon(), func() {
		a.openSettingsWindow()
	})
	settingBtn.Importance = widget.LowImportance
	railSize := canvas.NewRectangle(color.Transparent)
	railSize.SetMinSize(fyne.NewSize(58, 1))
	leftRail := container.NewMax(
		railSize,
		canvas.NewRectangle(leftGray),
		container.NewBorder(
			container.NewPadded(a.selfAvatar),
			container.NewCenter(settingBtn),
			nil,
			nil,
			nil,
		),
	)

	middlePanel := container.NewMax(
		canvas.NewRectangle(color.White),
		a.contactBox,
	)

	a.headerAvatar = avatarImage("", 44)
	a.chatHeader = widget.NewLabel("请选择联系人")
	a.chatHeader.TextStyle = fyne.TextStyle{Bold: true}
	a.chatSubHeader = widget.NewLabel("离线")
	profileBtn := widget.NewButtonWithIcon("资料", theme.InfoIcon(), func() { a.openPeerProfileDialog() })

	a.chatStream = container.NewVBox()
	a.chatScroll = container.NewVScroll(a.chatStream)
	a.chatScroll.Direction = container.ScrollVerticalOnly

	a.inputBox = newChatInput(func() { a.sendCurrentText() })
	a.inputBox.SetMinRowsVisible(5)
	a.inputBox.SetPlaceHolder("输入消息")

	emojiBtn := widget.NewButtonWithIcon("表情", theme.ContentAddIcon(), func() { a.openEmojiPicker() })
	fileBtn := widget.NewButtonWithIcon("文件", theme.FileIcon(), func() { a.sendCurrentFile() })
	imageBtn := widget.NewButtonWithIcon("图片", theme.FileImageIcon(), func() { a.sendCurrentImage() })
	a.statusBar = widget.NewLabel("")
	a.statusBar.Hide()

	headerRow := container.NewHBox(
		a.headerAvatar,
		container.NewVBox(a.chatHeader, a.chatSubHeader),
		layout.NewSpacer(),
		profileBtn,
	)
	actions := container.NewHBox(emojiBtn, fileBtn, imageBtn)
	composePanel := container.NewMax(
		canvas.NewRectangle(color.White),
		container.NewBorder(
			container.NewVBox(container.NewPadded(actions), lineWithColor(lineGray)),
			nil,
			nil,
			nil,
			container.NewPadded(a.inputBox),
		),
	)
	chatBodySplit := container.NewVSplit(
		container.NewMax(canvas.NewRectangle(rightGray), container.NewPadded(a.chatScroll)),
		composePanel,
	)
	chatBodySplit.Offset = 0.78
	a.chatPanel = container.NewBorder(
		container.NewVBox(container.NewPadded(headerRow), lineWithColor(lineGray)),
		nil,
		nil,
		nil,
		chatBodySplit,
	)
	a.rightHost = container.NewMax(blankRightPanel())

	mainSplit := container.NewHSplit(middlePanel, a.rightHost)
	mainSplit.Offset = 0.33

	root := container.NewBorder(
		nil,
		nil,
		container.NewBorder(nil, nil, nil, lineWithColor(lineGray), leftRail),
		nil,
		mainSplit,
	)
	return container.NewMax(canvas.NewRectangle(rightGray), root)
}

func (a *appState) openSettingsWindow() {
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
			a.applyAvatar(path, a.win)
		}, a.win)
	})
	clearAvatarBtn := widget.NewButton("移除头像", func() {
		a.cfg.Avatar = ""
		_ = a.saveConfig()
		a.refreshAvatarViews()
	})

	copyIDBtn := widget.NewButton("复制识别码", func() {
		a.win.Clipboard().SetContent(strings.TrimSpace(a.cfg.ID))
		a.pushStatus("已复制")
	})

	avatarRow := container.NewHBox(
		avatarPreview,
		container.NewVBox(pickAvatarBtn, clearAvatarBtn),
	)
	infoForm := widget.NewForm(
		widget.NewFormItem("昵称", nameInput),
		widget.NewFormItem("识别码", idInput),
	)
	content := container.NewPadded(widget.NewCard("", "", container.NewVBox(avatarRow, infoForm, copyIDBtn)))
	dlg := dialog.NewCustomConfirm("个人设置", "保存", "关闭", content, func(save bool) {
		if save {
			name := strings.TrimSpace(nameInput.Text)
			if name == "" {
				name = "LanUser"
			}
			a.cfg.Name = name
			if err := a.saveConfig(); err != nil {
				dialog.ShowError(err, a.win)
				return
			}
			if a.listenPort > 0 {
				a.broadcastHello(a.listenPort)
			}
			a.refreshContacts()
			a.refreshAvatarViews()
			a.mu.Lock()
			active := a.activeKey
			a.mu.Unlock()
			if active != "" {
				a.refreshHeaderByKey(active)
			}
			a.pushStatus("已保存")
		}
	}, a.win)
	dlg.SetOnClosed(func() {
		a.settingsAvatar = nil
	})
	dlg.Resize(fyne.NewSize(520, 420))
	dlg.Show()
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
	a.mu.Lock()
	active := a.activeKey
	a.mu.Unlock()
	a.safeUI(func() {
		if a.settingsAvatar != nil {
			setAvatarImage(a.settingsAvatar, path)
		}
		if a.selfAvatar != nil {
			setAvatarImage(a.selfAvatar, path)
		}
		if active == "" && a.chatHeader != nil {
			a.chatHeader.SetText("请选择联系人")
		}
		if active == "" && a.chatSubHeader != nil {
			a.chatSubHeader.SetText("离线")
		}
	})
}

func (a *appState) openPeerProfileDialog() {
	a.mu.Lock()
	key := a.activeKey
	p := a.peers[key]
	kp := a.knownPeers[key]
	a.mu.Unlock()
	if key == "" {
		return
	}
	id := strings.TrimSpace(kp.ID)
	name := strings.TrimSpace(kp.Name)
	addr := strings.TrimSpace(kp.Addr)
	if p != nil {
		if strings.TrimSpace(p.ID) != "" {
			id = strings.TrimSpace(p.ID)
		}
		if strings.TrimSpace(p.Name) != "" {
			name = strings.TrimSpace(p.Name)
		}
		if strings.TrimSpace(p.Addr) != "" {
			addr = strings.TrimSpace(p.Addr)
		}
	}
	if id == "" {
		id, _ = splitPeerKey(key)
	}
	if name == "" {
		name = id
	}

	nameText := widget.NewEntry()
	nameText.SetText(name)
	nameText.Disable()

	ipText := widget.NewEntry()
	ipText.SetText(addr)
	ipText.Disable()

	idText := widget.NewEntry()
	idText.SetText(id)
	idText.Disable()

	remarkInput := widget.NewEntry()
	remarkInput.SetText(a.getRemark(id))

	copyIPBtn := widget.NewButton("复制IP", func() {
		a.win.Clipboard().SetContent(addr)
		a.pushStatus("已复制")
	})
	copyIDBtn := widget.NewButton("复制识别码", func() {
		a.win.Clipboard().SetContent(id)
		a.pushStatus("已复制")
	})

	content := container.NewPadded(container.NewVBox(
		widget.NewForm(
			widget.NewFormItem("昵称", nameText),
			widget.NewFormItem("IP", ipText),
			widget.NewFormItem("识别码", idText),
			widget.NewFormItem("好友备注", remarkInput),
		),
		container.NewHBox(copyIPBtn, copyIDBtn),
	))
	dlg := dialog.NewCustomConfirm("好友资料", "保存", "关闭", content, func(confirm bool) {
		if confirm {
			a.setRemark(id, remarkInput.Text)
			a.refreshContacts()
			a.refreshHeaderByKey(key)
		}
	}, a.win)
	dlg.Resize(fyne.NewSize(520, 320))
	dlg.Show()
}

func (a *appState) openEmojiPicker() {
	emojis := []string{
		"😀", "😁", "😂", "🤣", "😊", "😍", "😘", "😎",
		"🤔", "😴", "😭", "😡", "😱", "🥳", "🤝", "👍",
		"👏", "🙏", "💯", "🔥", "❤️", "💙", "🎉", "🌟",
	}
	var dlg *dialog.CustomDialog
	grid := container.NewGridWithColumns(8)
	for _, e := range emojis {
		emoji := e
		grid.Add(newEmojiTile(emoji, func(string) {
			a.insertToInput(emoji)
			if dlg != nil {
				dlg.Hide()
			}
		}))
	}
	dlg = dialog.NewCustom("选择表情", "关闭", container.NewPadded(grid), a.win)
	dlg.Resize(fyne.NewSize(520, 260))
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

func (a *appState) peerDisplayByKey(key string) (string, bool) {
	a.mu.Lock()
	p := a.peers[key]
	kp := a.knownPeers[key]
	a.mu.Unlock()
	if p != nil {
		return a.displayNameForPeer(p), true
	}
	id := strings.TrimSpace(kp.ID)
	if id == "" {
		id, _ = splitPeerKey(key)
	}
	if id == "" {
		return "", false
	}
	if remark := a.getRemark(id); remark != "" {
		return remark, false
	}
	if strings.TrimSpace(kp.Name) != "" {
		return strings.TrimSpace(kp.Name), false
	}
	return id, false
}

func (a *appState) refreshHeaderByKey(key string) {
	name, online := a.peerDisplayByKey(key)
	if name == "" {
		return
	}
	status := "离线"
	if online {
		status = "在线"
	}
	a.safeUI(func() {
		a.chatHeader.SetText(name)
		a.chatSubHeader.SetText(status)
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
					}
				}
				a.mu.Unlock()
				if changed {
					a.refreshContacts()
					a.mu.Lock()
					active := a.activeKey
					a.mu.Unlock()
					if active != "" {
						a.refreshHeaderByKey(active)
					}
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
	changed := a.upsertKnownPeerLocked(in.Key, knownPeer{
		ID:         in.ID,
		InstanceID: in.InstanceID,
		Name:       in.Name,
		Addr:       in.Addr,
	})
	a.mu.Unlock()
	if changed {
		_ = a.saveKnownPeers()
	}
	a.refreshContacts()
	a.mu.Lock()
	active := a.activeKey
	a.mu.Unlock()
	if active == in.Key {
		a.refreshHeaderByKey(active)
	}
}

func (a *appState) refreshContacts() {
	a.mu.Lock()
	peers := make(map[string]peer, len(a.peers))
	for k, p := range a.peers {
		if p != nil {
			peers[k] = *p
		}
	}
	known := make(map[string]knownPeer, len(a.knownPeers))
	for k, v := range a.knownPeers {
		known[k] = v
	}
	unread := make(map[string]int, len(a.unread))
	for k, v := range a.unread {
		unread[k] = v
	}
	a.mu.Unlock()

	keys := map[string]struct{}{}
	for k := range peers {
		keys[k] = struct{}{}
	}
	for k := range known {
		keys[k] = struct{}{}
	}

	now := time.Now()
	rows := make([]peerRow, 0, len(keys))
	for key := range keys {
		p, online := peers[key]
		kp := known[key]
		if online && now.Sub(p.LastSeen) > presenceTTL {
			online = false
		}
		id := strings.TrimSpace(kp.ID)
		if online && strings.TrimSpace(p.ID) != "" {
			id = strings.TrimSpace(p.ID)
		}
		if id == "" {
			id, _ = splitPeerKey(key)
		}
		name := ""
		if online {
			name = a.displayNameForPeer(&p)
		} else {
			remark := a.getRemark(id)
			if remark != "" {
				name = remark
			} else if strings.TrimSpace(kp.Name) != "" {
				name = strings.TrimSpace(kp.Name)
			} else {
				name = id
			}
		}
		preview := compactLine(strings.TrimSpace(kp.LastText), 48)
		if preview == "" {
			preview = "暂无聊天记录"
		}
		unreadCount := unread[key]
		if unreadCount > 0 {
			preview = fmt.Sprintf("(%d) %s", unreadCount, preview)
		}
		rows = append(rows, peerRow{
			Key:        key,
			Name:       name,
			Preview:    preview,
			Online:     online,
			Unread:     unreadCount,
			LastSentAt: kp.LastSentAt,
		})
	}
	sort.Slice(rows, func(i, j int) bool {
		if rows[i].Online != rows[j].Online {
			return rows[i].Online
		}
		if rows[i].LastSentAt != rows[j].LastSentAt {
			return rows[i].LastSentAt > rows[j].LastSentAt
		}
		return strings.ToLower(rows[i].Name) < strings.ToLower(rows[j].Name)
	})

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
	row := a.peerRows[index]
	key := row.Key
	a.activeKey = key
	a.mu.Unlock()
	a.showChatPanel()
	status := "离线"
	if row.Online {
		status = "在线"
	}
	a.safeUI(func() {
		a.chatHeader.SetText(row.Name)
		a.chatSubHeader.SetText(status)
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
		items = append(items, a.renderBubbleMessage(m))
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

func (a *appState) renderBubbleMessage(m historyLine) fyne.CanvasObject {
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
		bgColor = color.NRGBA{R: 248, G: 249, B: 251, A: 255}
	}
	bg := canvas.NewRectangle(bgColor)
	bg.CornerRadius = 10
	bubble := container.NewMax(
		bg,
		container.NewPadded(bodyLabel),
	)

	var metaRow *fyne.Container
	var bodyRow *fyne.Container
	guard := float32(260)
	if a.chatScroll != nil {
		if w := a.chatScroll.Size().Width; w > 0 {
			maxBubble := w * 0.68
			if maxBubble > 560 {
				maxBubble = 560
			}
			if maxBubble < 220 {
				maxBubble = 220
			}
			if g := w - maxBubble - 40; g > 0 {
				guard = g
			} else {
				guard = 0
			}
		}
	}
	maxGuard := spacerBox(guard)
	if outgoing {
		metaRow = container.NewHBox(layout.NewSpacer(), metaLabel)
		bodyRow = container.NewHBox(maxGuard, layout.NewSpacer(), container.NewPadded(bubble), spacerBox(20))
	} else {
		metaRow = container.NewHBox(spacerBox(20), metaLabel, layout.NewSpacer())
		bodyRow = container.NewHBox(spacerBox(20), container.NewPadded(bubble), layout.NewSpacer(), maxGuard)
	}
	return container.NewVBox(metaRow, bodyRow)
}

func spacerBox(w float32) *canvas.Rectangle {
	box := canvas.NewRectangle(color.Transparent)
	box.SetMinSize(fyne.NewSize(w, 1))
	return box
}

func lineWithColor(c color.Color) *canvas.Rectangle {
	line := canvas.NewRectangle(c)
	line.SetMinSize(fyne.NewSize(1, 1))
	return line
}

func blankRightPanel() fyne.CanvasObject {
	bg := canvas.NewRectangle(color.NRGBA{R: 244, G: 246, B: 248, A: 255})
	return container.NewMax(bg, layout.NewSpacer())
}

func (a *appState) showChatPanel() {
	a.safeUI(func() {
		if a.rightHost == nil || a.chatPanel == nil {
			return
		}
		a.rightHost.Objects = []fyne.CanvasObject{a.chatPanel}
		a.rightHost.Refresh()
	})
}

func (a *appState) showBlankPanel() {
	a.safeUI(func() {
		if a.rightHost == nil {
			return
		}
		a.rightHost.Objects = []fyne.CanvasObject{blankRightPanel()}
		a.rightHost.Refresh()
	})
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
	path := a.historyPath(peerKey)
	list := []historyLine{}
	if b, err := os.ReadFile(path); err == nil {
		_ = json.Unmarshal(b, &list)
	}
	list = append(list, line)
	b, _ := json.MarshalIndent(list, "", "  ")
	_ = os.WriteFile(path, b, 0o644)
	changed := a.upsertKnownPeerLocked(peerKey, knownPeer{
		LastText:   line.Text,
		LastSentAt: line.SentAt,
	})
	a.mu.Unlock()
	if changed {
		_ = a.saveKnownPeers()
	}
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

func splitPeerKey(key string) (string, string) {
	parts := strings.SplitN(key, "@", 2)
	if len(parts) == 2 {
		return parts[0], parts[1]
	}
	if len(parts) == 1 {
		return parts[0], ""
	}
	return "", ""
}

func compactLine(s string, limit int) string {
	s = strings.TrimSpace(s)
	s = strings.ReplaceAll(s, "\r", " ")
	s = strings.ReplaceAll(s, "\n", " ")
	for strings.Contains(s, "  ") {
		s = strings.ReplaceAll(s, "  ", " ")
	}
	if len(s) <= limit {
		return s
	}
	if limit <= 3 {
		return s[:limit]
	}
	return s[:limit-3] + "..."
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
