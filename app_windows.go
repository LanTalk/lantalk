//go:build windows

package main

import (
	"crypto/aes"
	"crypto/cipher"
	"crypto/rand"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"sync"
	"time"

	"github.com/lxn/walk"
	. "github.com/lxn/walk/declarative"
	"golang.org/x/crypto/curve25519"
	"golang.org/x/crypto/hkdf"
)

const (
	discoveryPort = 43999
	discoveryHost = "239.255.77.77"
	presenceTTL   = 15 * time.Second
	announceEvery = 3 * time.Second
)

type config struct {
	ID         string `json:"id"`
	Name       string `json:"name"`
	PublicKey  string `json:"publicKey"`
	PrivateKey string `json:"privateKey"`
}

type peer struct {
	ID        string
	Name      string
	Addr      string
	Port      int
	PublicKey string
	LastSeen  time.Time
}

type discoveryPacket struct {
	Type      string `json:"type"`
	ID        string `json:"id"`
	Name      string `json:"name"`
	PublicKey string `json:"publicKey"`
	Port      int    `json:"port"`
	Time      int64  `json:"time"`
}

type plainMessage struct {
	ID     string `json:"id"`
	Text   string `json:"text"`
	SentAt int64  `json:"sentAt"`
}

type encryptedMessage struct {
	Salt  string `json:"salt"`
	Nonce string `json:"nonce"`
	Data  string `json:"data"`
}

type chatEnvelope struct {
	Type       string           `json:"type"`
	FromID     string           `json:"fromId"`
	FromName   string           `json:"fromName"`
	FromPubKey string           `json:"fromPubKey"`
	Payload    encryptedMessage `json:"payload"`
}

type historyLine struct {
	Direction string `json:"direction"`
	From      string `json:"from"`
	Text      string `json:"text"`
	SentAt    int64  `json:"sentAt"`
}

type appState struct {
	baseDir  string
	dataDir  string
	chatsDir string
	cfg      config

	mu      sync.Mutex
	peers   map[string]*peer
	peerIDs []string
	active  string

	tcpListener net.Listener
	udpConn     *net.UDPConn

	mw         *walk.MainWindow
	contactBox *walk.ListBox
	nameInput  *walk.LineEdit
	selfLabel  *walk.Label
	chatLabel  *walk.Label
	chatView   *walk.TextEdit
	inputBox   *walk.TextEdit
	statusBar  *walk.Label
}

func newApp(baseDir string) *appState {
	return &appState{
		baseDir:  baseDir,
		dataDir:  filepath.Join(baseDir, "data"),
		chatsDir: filepath.Join(baseDir, "data", "chats"),
		peers:    map[string]*peer{},
	}
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
	if err := a.startDiscovery(port); err != nil {
		return err
	}
	a.pushStatus("已启动，正在发现局域网好友")

	exit := a.mw.Run()
	a.shutdown()
	if exit != 0 {
		return fmt.Errorf("程序退出码: %d", exit)
	}
	return nil
}

func (a *appState) initStorage() error {
	if err := os.MkdirAll(a.chatsDir, 0o755); err != nil {
		return err
	}
	cfgPath := filepath.Join(a.dataDir, "config.json")
	if b, err := os.ReadFile(cfgPath); err == nil {
		if err := json.Unmarshal(b, &a.cfg); err != nil {
			return err
		}
		if a.cfg.ID != "" && a.cfg.PrivateKey != "" && a.cfg.PublicKey != "" {
			if a.cfg.Name == "" {
				a.cfg.Name = "LanUser"
			}
			return nil
		}
	}

	host, _ := os.Hostname()
	if host == "" {
		host = "LanUser"
	}
	pub, priv, err := generateKeyPair()
	if err != nil {
		return err
	}
	a.cfg = config{
		ID:         randomID(8),
		Name:       host,
		PublicKey:  pub,
		PrivateKey: priv,
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
	if err := (MainWindow{
		AssignTo: &a.mw,
		Title:    "LanTalk - 经典局域网聊天",
		Size:     Size{Width: 980, Height: 680},
		MinSize:  Size{Width: 860, Height: 560},
		Layout:   HBox{MarginsZero: true, Spacing: 0},
		Children: []Widget{
			Composite{
				MinSize:    Size{Width: 260},
				Background: SolidColorBrush{Color: walk.RGB(232, 243, 255)},
				Layout:     VBox{Margins: Margins{Left: 14, Top: 14, Right: 14, Bottom: 14}, Spacing: 8},
				Children: []Widget{
					Composite{
						Background: SolidColorBrush{Color: walk.RGB(56, 142, 255)},
						Layout:     VBox{Margins: Margins{Left: 10, Top: 8, Right: 10, Bottom: 8}},
						Children: []Widget{
							Label{Text: "LanTalk", Font: Font{PointSize: 13, Family: "Microsoft YaHei UI", Bold: true}, TextColor: walk.RGB(255, 255, 255)},
							Label{AssignTo: &a.selfLabel, Text: "昵称: -", TextColor: walk.RGB(236, 244, 255)},
						},
					},
					LineEdit{AssignTo: &a.nameInput, Text: a.cfg.Name},
					PushButton{
						Text: "保存昵称",
						OnClicked: func() {
							name := strings.TrimSpace(a.nameInput.Text())
							if name == "" {
								return
							}
							a.cfg.Name = name
							_ = a.saveConfig()
							a.selfLabel.SetText("昵称: " + a.cfg.Name)
							a.pushStatus("昵称已更新")
						},
					},
					Label{Text: "在线好友", Font: Font{PointSize: 10, Bold: true}},
					ListBox{
						AssignTo: &a.contactBox,
						Model:    []string{},
						OnCurrentIndexChanged: func() {
							a.selectPeer(a.contactBox.CurrentIndex())
						},
					},
				},
			},
			Composite{
				Layout: VBox{Margins: Margins{Left: 16, Top: 12, Right: 16, Bottom: 12}, Spacing: 8},
				Children: []Widget{
					Label{AssignTo: &a.chatLabel, Text: "请选择左侧好友开始聊天", Font: Font{PointSize: 11, Bold: true}},
					TextEdit{AssignTo: &a.chatView, ReadOnly: true, VScroll: true},
					TextEdit{AssignTo: &a.inputBox, VScroll: true},
					Composite{
						Layout: HBox{MarginsZero: true},
						Children: []Widget{
							HSpacer{},
							PushButton{
								Text: "发送",
								OnClicked: func() {
									a.sendCurrentText()
								},
							},
						},
					},
					Label{AssignTo: &a.statusBar, Text: "准备就绪", TextColor: walk.RGB(90, 104, 120)},
				},
			},
		},
	}).Create(); err != nil {
		return err
	}

	a.selfLabel.SetText("昵称: " + a.cfg.Name)
	return nil
}

func (a *appState) startTCPServer() (int, error) {
	ln, err := net.Listen("tcp4", "0.0.0.0:0")
	if err != nil {
		return 0, err
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
	var env chatEnvelope
	dec := json.NewDecoder(io.LimitReader(conn, 1<<20))
	if err := dec.Decode(&env); err != nil {
		return
	}
	if env.Type != "chat" || env.FromID == a.cfg.ID || env.FromPubKey == "" {
		return
	}

	host, _, _ := net.SplitHostPort(conn.RemoteAddr().String())
	a.upsertPeer(peer{
		ID:        env.FromID,
		Name:      env.FromName,
		Addr:      host,
		PublicKey: env.FromPubKey,
		LastSeen:  time.Now(),
	})

	plain, err := decrypt(env.FromPubKey, a.cfg.PrivateKey, env.Payload)
	if err != nil {
		return
	}
	line := historyLine{Direction: "in", From: env.FromName, Text: plain.Text, SentAt: plain.SentAt}
	a.appendHistory(env.FromID, line)
	a.refreshChatIfActive(env.FromID)
}

func (a *appState) sendCurrentText() {
	text := strings.TrimSpace(a.inputBox.Text())
	if text == "" {
		return
	}
	a.mu.Lock()
	peerID := a.active
	p := a.peers[peerID]
	a.mu.Unlock()
	if peerID == "" || p == nil {
		a.pushStatus("请先选择在线好友")
		return
	}

	plain := plainMessage{ID: randomID(8), Text: text, SentAt: time.Now().Unix()}
	payload, err := encrypt(p.PublicKey, a.cfg.PrivateKey, plain)
	if err != nil {
		a.pushStatus("加密失败: " + err.Error())
		return
	}
	env := chatEnvelope{
		Type:       "chat",
		FromID:     a.cfg.ID,
		FromName:   a.cfg.Name,
		FromPubKey: a.cfg.PublicKey,
		Payload:    payload,
	}
	if err := sendEnvelope(*p, env); err != nil {
		a.pushStatus("发送失败: " + err.Error())
		return
	}

	a.inputBox.SetText("")
	a.appendHistory(peerID, historyLine{Direction: "out", From: a.cfg.Name, Text: text, SentAt: plain.SentAt})
	a.refreshChatIfActive(peerID)
	a.pushStatus("已发送")
}

func (a *appState) startDiscovery(chatPort int) error {
	group := &net.UDPAddr{IP: net.ParseIP(discoveryHost), Port: discoveryPort}
	conn, err := net.ListenMulticastUDP("udp4", nil, group)
	if err != nil {
		return err
	}
	_ = conn.SetReadBuffer(64 * 1024)
	a.udpConn = conn

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
			if p.Type != "hello" || p.ID == a.cfg.ID || p.PublicKey == "" {
				continue
			}
			a.upsertPeer(peer{
				ID:        p.ID,
				Name:      p.Name,
				Addr:      addr.IP.String(),
				Port:      p.Port,
				PublicKey: p.PublicKey,
				LastSeen:  time.Now(),
			})
		}
	}()

	go func() {
		ticker := time.NewTicker(announceEvery)
		defer ticker.Stop()
		for {
			a.broadcastHello(chatPort)
			<-ticker.C
		}
	}()

	go func() {
		ticker := time.NewTicker(5 * time.Second)
		defer ticker.Stop()
		for range ticker.C {
			changed := false
			now := time.Now()
			a.mu.Lock()
			for id, p := range a.peers {
				if now.Sub(p.LastSeen) > presenceTTL {
					delete(a.peers, id)
					changed = true
					if a.active == id {
						a.active = ""
					}
				}
			}
			a.mu.Unlock()
			if changed {
				a.refreshContacts()
			}
		}
	}()

	return nil
}

func (a *appState) broadcastHello(chatPort int) {
	pkt := discoveryPacket{
		Type:      "hello",
		ID:        a.cfg.ID,
		Name:      a.cfg.Name,
		PublicKey: a.cfg.PublicKey,
		Port:      chatPort,
		Time:      time.Now().Unix(),
	}
	b, _ := json.Marshal(pkt)
	conn, err := net.DialUDP("udp4", nil, &net.UDPAddr{IP: net.ParseIP(discoveryHost), Port: discoveryPort})
	if err != nil {
		return
	}
	defer conn.Close()
	_, _ = conn.Write(b)
}

func (a *appState) upsertPeer(in peer) {
	if in.ID == "" || in.ID == a.cfg.ID {
		return
	}
	a.mu.Lock()
	exist := a.peers[in.ID]
	if exist != nil && exist.PublicKey != "" && exist.PublicKey != in.PublicKey {
		a.mu.Unlock()
		return
	}
	copyPeer := in
	if copyPeer.Name == "" {
		copyPeer.Name = in.ID
	}
	if exist != nil {
		exist.Name = copyPeer.Name
		exist.Addr = copyPeer.Addr
		exist.Port = copyPeer.Port
		exist.LastSeen = copyPeer.LastSeen
	} else {
		a.peers[in.ID] = &copyPeer
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
	a.mu.Unlock()

	sort.Slice(pairs, func(i, j int) bool {
		return strings.ToLower(pairs[i].Name) < strings.ToLower(pairs[j].Name)
	})
	items := make([]string, 0, len(pairs))
	ids := make([]string, 0, len(pairs))
	for _, p := range pairs {
		items = append(items, fmt.Sprintf("%s  (%s)", p.Name, p.Addr))
		ids = append(ids, p.ID)
	}

	a.safeUI(func() {
		a.peerIDs = ids
		a.contactBox.SetModel(items)
	})
}

func (a *appState) selectPeer(index int) {
	if index < 0 || index >= len(a.peerIDs) {
		return
	}
	peerID := a.peerIDs[index]
	a.mu.Lock()
	p := a.peers[peerID]
	a.active = peerID
	a.mu.Unlock()
	if p == nil {
		return
	}
	a.chatLabel.SetText("正在与 " + p.Name + " 对话")
	a.renderHistory(peerID)
}

func (a *appState) renderHistory(peerID string) {
	history, _ := a.loadHistory(peerID)
	var b strings.Builder
	for _, m := range history {
		name := m.From
		if m.Direction == "out" {
			name = "我"
		}
		b.WriteString("[")
		b.WriteString(time.Unix(m.SentAt, 0).Format("15:04:05"))
		b.WriteString("] ")
		b.WriteString(name)
		b.WriteString(": ")
		b.WriteString(m.Text)
		b.WriteString("\r\n")
	}
	a.safeUI(func() {
		a.chatView.SetText(b.String())
		a.chatView.SetTextSelection(len(a.chatView.Text()), len(a.chatView.Text()))
	})
}

func (a *appState) refreshChatIfActive(peerID string) {
	a.mu.Lock()
	active := a.active
	a.mu.Unlock()
	if active == peerID {
		a.renderHistory(peerID)
	}
}

func (a *appState) appendHistory(peerID string, line historyLine) {
	a.mu.Lock()
	defer a.mu.Unlock()
	path := filepath.Join(a.chatsDir, peerID+".json")
	list := []historyLine{}
	if b, err := os.ReadFile(path); err == nil {
		_ = json.Unmarshal(b, &list)
	}
	list = append(list, line)
	b, _ := json.MarshalIndent(list, "", "  ")
	_ = os.WriteFile(path, b, 0o644)
}

func (a *appState) loadHistory(peerID string) ([]historyLine, error) {
	path := filepath.Join(a.chatsDir, peerID+".json")
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

func (a *appState) pushStatus(text string) {
	a.safeUI(func() {
		a.statusBar.SetText(text)
	})
}

func (a *appState) safeUI(fn func()) {
	if a.mw == nil {
		return
	}
	a.mw.Synchronize(fn)
}

func (a *appState) shutdown() {
	if a.udpConn != nil {
		_ = a.udpConn.Close()
	}
	if a.tcpListener != nil {
		_ = a.tcpListener.Close()
	}
}

func sendEnvelope(p peer, env chatEnvelope) error {
	if p.Port == 0 || p.Addr == "" {
		return errors.New("好友地址不可用")
	}
	conn, err := net.DialTimeout("tcp4", fmt.Sprintf("%s:%d", p.Addr, p.Port), 2*time.Second)
	if err != nil {
		return err
	}
	defer conn.Close()
	conn.SetDeadline(time.Now().Add(3 * time.Second))
	return json.NewEncoder(conn).Encode(env)
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

func encrypt(peerPubB64, selfPrivB64 string, plain plainMessage) (encryptedMessage, error) {
	plainBytes, err := json.Marshal(plain)
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
	return encryptedMessage{
		Salt:  base64.StdEncoding.EncodeToString(salt),
		Nonce: base64.StdEncoding.EncodeToString(nonce),
		Data:  base64.StdEncoding.EncodeToString(cipherText),
	}, nil
}

func decrypt(senderPubB64, selfPrivB64 string, c encryptedMessage) (plainMessage, error) {
	salt, err := base64.StdEncoding.DecodeString(c.Salt)
	if err != nil {
		return plainMessage{}, err
	}
	nonce, err := base64.StdEncoding.DecodeString(c.Nonce)
	if err != nil {
		return plainMessage{}, err
	}
	cipherText, err := base64.StdEncoding.DecodeString(c.Data)
	if err != nil {
		return plainMessage{}, err
	}
	key, err := deriveKey(selfPrivB64, senderPubB64, salt)
	if err != nil {
		return plainMessage{}, err
	}
	block, err := aes.NewCipher(key)
	if err != nil {
		return plainMessage{}, err
	}
	gcm, err := cipher.NewGCM(block)
	if err != nil {
		return plainMessage{}, err
	}
	plainBytes, err := gcm.Open(nil, nonce, cipherText, nil)
	if err != nil {
		return plainMessage{}, err
	}
	var p plainMessage
	if err := json.Unmarshal(plainBytes, &p); err != nil {
		return plainMessage{}, err
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

func randomID(n int) string {
	b := make([]byte, n)
	if _, err := rand.Read(b); err != nil {
		return fmt.Sprintf("fallback-%d", time.Now().UnixNano())
	}
	return hex.EncodeToString(b)
}

func runEntry() {
	exe, err := os.Executable()
	if err != nil {
		_ = walk.MsgBox(nil, "LanTalk", "无法获取程序路径: "+err.Error(), walk.MsgBoxIconError)
		return
	}
	app := newApp(filepath.Dir(exe))
	if err := app.run(); err != nil {
		_ = walk.MsgBox(nil, "LanTalk", err.Error(), walk.MsgBoxIconError)
	}
}
