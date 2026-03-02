#include "chat_window.h"

#include <QAbstractSocket>
#include <QBuffer>
#include <QCloseEvent>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QHostAddress>
#include <QIcon>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLinearGradient>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QMetaObject>
#include <QNetworkInterface>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPushButton>
#include <QRandomGenerator>
#include <QScrollBar>
#include <QSet>
#include <QSignalBlocker>
#include <QStringList>
#include <QTextBrowser>
#include <QTextEdit>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
QString htmlEscape(const QString& value) {
    return value.toHtmlEscaped();
}

qint64 nowMs() {
    return QDateTime::currentMSecsSinceEpoch();
}

QString timeText(qint64 ms) {
    return QDateTime::fromMSecsSinceEpoch(ms).toString("MM-dd HH:mm");
}

QString safeFileToken(const QString& source) {
    QString out;
    out.reserve(source.size());
    for (const QChar ch : source) {
        if (ch.isLetterOrNumber() || ch == '_' || ch == '-') {
            out.push_back(ch);
        } else {
            out.push_back('_');
        }
    }
    if (out.isEmpty()) {
        return "unknown";
    }
    return out;
}

template <typename ContactLike>
qint64 lastMessageTs(const ContactLike& contact) {
    if (contact.messages.empty()) {
        return contact.lastSeenMs;
    }
    return contact.messages.back().timestampMs;
}

QPixmap makeDefaultAvatar(const QString& seed, int size) {
    QPixmap pix(size, size);
    pix.fill(Qt::transparent);

    const uint32_t hash = qHash(seed.isEmpty() ? QStringLiteral("L") : seed);
    const int hue = static_cast<int>(hash % 360U);
    const QColor top = QColor::fromHsl(hue, 170, 145);
    const QColor bottom = QColor::fromHsl((hue + 26) % 360, 150, 120);

    QPainter painter(&pix);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QRectF outerRect(1, 1, size - 2.0, size - 2.0);
    QLinearGradient background(0, 0, size, size);
    background.setColorAt(0.0, top);
    background.setColorAt(1.0, bottom);
    painter.setPen(Qt::NoPen);
    painter.setBrush(background);
    painter.drawEllipse(outerRect);

    painter.setPen(QPen(QColor(255, 255, 255, 105), std::max(1, size / 24)));
    painter.setBrush(Qt::NoBrush);
    painter.drawEllipse(outerRect.adjusted(1.0, 1.0, -1.0, -1.0));

    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(255, 255, 255, 46));
    painter.drawEllipse(QRectF(size * 0.14, size * 0.10, size * 0.58, size * 0.32));

    const QColor glyph(255, 255, 255, 225);
    painter.setBrush(glyph);
    painter.setPen(Qt::NoPen);
    painter.drawEllipse(QRectF(size * 0.365, size * 0.25, size * 0.27, size * 0.27));

    QPainterPath shoulders;
    shoulders.moveTo(size * 0.20, size * 0.83);
    shoulders.quadTo(size * 0.50, size * 0.54, size * 0.80, size * 0.83);
    shoulders.lineTo(size * 0.80, size * 0.95);
    shoulders.lineTo(size * 0.20, size * 0.95);
    shoulders.closeSubpath();
    painter.drawPath(shoulders);
    return pix;
}

QPixmap makeRoundAvatar(const QImage& image, int size, const QString& fallbackSeed) {
    if (image.isNull()) {
        return makeDefaultAvatar(fallbackSeed, size);
    }

    QImage scaled = image.scaled(size, size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    if (scaled.isNull()) {
        return makeDefaultAvatar(fallbackSeed, size);
    }

    QPixmap pix(size, size);
    pix.fill(Qt::transparent);

    QPainter painter(&pix);
    painter.setRenderHint(QPainter::Antialiasing, true);
    QPainterPath path;
    path.addEllipse(0, 0, size, size);
    painter.setClipPath(path);
    painter.drawImage(0, 0, scaled);
    return pix;
}

QIcon makeAppIcon() {
    QPixmap pix(128, 128);
    pix.fill(Qt::transparent);

    QPainter painter(&pix);
    painter.setRenderHint(QPainter::Antialiasing, true);

    QLinearGradient background(8, 8, 120, 120);
    background.setColorAt(0.0, QColor(20, 90, 240));
    background.setColorAt(0.52, QColor(44, 124, 255));
    background.setColorAt(1.0, QColor(24, 182, 216));
    painter.setPen(Qt::NoPen);
    painter.setBrush(background);
    painter.drawRoundedRect(4, 4, 120, 120, 30, 30);

    painter.setBrush(QColor(255, 255, 255, 36));
    painter.drawEllipse(QRectF(16, 14, 92, 44));

    QPainterPath bubbleLeft;
    bubbleLeft.addRoundedRect(QRectF(20, 30, 58, 44), 14, 14);
    bubbleLeft.moveTo(34, 74);
    bubbleLeft.lineTo(27, 92);
    bubbleLeft.lineTo(46, 79);
    bubbleLeft.closeSubpath();
    painter.setBrush(QColor(255, 255, 255, 245));
    painter.drawPath(bubbleLeft);

    QPainterPath bubbleRight;
    bubbleRight.addRoundedRect(QRectF(53, 49, 56, 44), 14, 14);
    bubbleRight.moveTo(88, 93);
    bubbleRight.lineTo(98, 106);
    bubbleRight.lineTo(82, 97);
    bubbleRight.closeSubpath();
    painter.setBrush(QColor(227, 242, 255, 238));
    painter.drawPath(bubbleRight);

    painter.setPen(QPen(QColor(36, 125, 255), 4, Qt::SolidLine, Qt::RoundCap));
    painter.drawLine(QPointF(36, 51), QPointF(60, 51));
    painter.drawLine(QPointF(36, 62), QPointF(54, 62));
    painter.drawLine(QPointF(66, 69), QPointF(90, 69));
    painter.drawLine(QPointF(66, 80), QPointF(84, 80));
    return QIcon(pix);
}

constexpr char kBlobMagic[] = "LTC2";

QByteArray nonceToBytes(uint64_t nonce) {
    QByteArray out;
    out.resize(8);
    for (int i = 0; i < 8; ++i) {
        out[i] = static_cast<char>((nonce >> (i * 8U)) & 0xFFU);
    }
    return out;
}

uint64_t bytesToNonce(const QByteArray& in, int offset) {
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(static_cast<uint8_t>(in[offset + i])) << (i * 8U);
    }
    return value;
}

QStringList builtInAvatarPaths() {
    QStringList out;
    out.reserve(12);
    for (int i = 1; i <= 12; ++i) {
        out.push_back(QString(":/avatars/default_%1.png").arg(i));
    }
    return out;
}

QImage decodeAvatarPayload(const QString& payload) {
    const QByteArray packed = QByteArray::fromBase64(payload.toLatin1());
    if (packed.isEmpty()) {
        return {};
    }
    QImage image;
    image.loadFromData(packed);
    return image;
}

QIcon makeContactAvatarIcon(const QString& avatarPayload, const QString& fallbackSeed, bool online) {
    const QImage image = decodeAvatarPayload(avatarPayload);
    QPixmap base = makeRoundAvatar(image, 30, fallbackSeed);

    QPainter painter(&base);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QRectF dotRect(20.0, 20.0, 10.0, 10.0);
    painter.setPen(QPen(Qt::white, 1.2));
    painter.setBrush(online ? QColor(34, 197, 94) : QColor(148, 163, 184));
    painter.drawEllipse(dotRect);
    return QIcon(base);
}
}  // namespace

ChatWindow::ChatWindow() {
    setupUi();
    bindEvents();

    app_.setEventCallback([](const std::string&) {});
    app_.setMessageCallback([this](const MessageEvent& event) {
        QMetaObject::invokeMethod(
            this,
            [this, event]() {
                handleMessageEvent(event);
            },
            Qt::QueuedConnection);
    });

    if (!app_.init()) {
        throw std::runtime_error(app_.lastError().empty() ? "初始化失败。" : app_.lastError());
    }
    if (!app_.startAsync()) {
        throw std::runtime_error("网络服务启动失败。");
    }

    ensureDefaultAvatarLibrary();
    loadProfile();
    if (selfAvatarPath_.isEmpty() || QImage(selfAvatarPath_).isNull()) {
        if (!defaultAvatarPaths_.isEmpty()) {
            const int pick = QRandomGenerator::global()->bounded(defaultAvatarPaths_.size());
            selfAvatarPath_ = defaultAvatarPaths_.at(pick);
            saveProfile();
        }
    }
    syncLocalAvatarToNetwork();
    applySelfAvatar();
    loadContacts();
    refreshOnlinePeers();
    updateChatHeader();

    refreshTimer_ = new QTimer(this);
    refreshTimer_->setInterval(1000);
    connect(refreshTimer_, &QTimer::timeout, this, [this]() { refreshOnlinePeers(); });
    refreshTimer_->start();
}

ChatWindow::~ChatWindow() {
    app_.setMessageCallback(nullptr);
    app_.setEventCallback(nullptr);
    app_.shutdown();
}

void ChatWindow::closeEvent(QCloseEvent* event) {
    app_.setMessageCallback(nullptr);
    app_.setEventCallback(nullptr);
    app_.shutdown();
    event->accept();
}

void ChatWindow::changeEvent(QEvent* event) {
    if (event != nullptr && event->type() == QEvent::WindowStateChange && maxBtn_ != nullptr) {
        maxBtn_->setText(isMaximized() ? QString(QChar(0xE923)) : QString(QChar(0xE922)));
    }
    QMainWindow::changeEvent(event);
}

bool ChatWindow::nativeEvent(const QByteArray& eventType, void* message, qintptr* result) {
#ifdef Q_OS_WIN
    Q_UNUSED(eventType);
    MSG* msg = static_cast<MSG*>(message);
    if (msg != nullptr && msg->message == WM_NCHITTEST) {
        const POINT cursor{
            static_cast<short>(LOWORD(msg->lParam)),
            static_cast<short>(HIWORD(msg->lParam)),
        };
        const QPoint globalPos(cursor.x, cursor.y);

        RECT winRect{};
        ::GetWindowRect(reinterpret_cast<HWND>(winId()), &winRect);
        const int border = 8;

        if (!isMaximized()) {
            const bool left = cursor.x >= winRect.left && cursor.x < winRect.left + border;
            const bool right = cursor.x <= winRect.right && cursor.x > winRect.right - border;
            const bool top = cursor.y >= winRect.top && cursor.y < winRect.top + border;
            const bool bottom = cursor.y <= winRect.bottom && cursor.y > winRect.bottom - border;
            if (left && top) {
                *result = HTTOPLEFT;
                return true;
            }
            if (right && top) {
                *result = HTTOPRIGHT;
                return true;
            }
            if (left && bottom) {
                *result = HTBOTTOMLEFT;
                return true;
            }
            if (right && bottom) {
                *result = HTBOTTOMRIGHT;
                return true;
            }
            if (left) {
                *result = HTLEFT;
                return true;
            }
            if (right) {
                *result = HTRIGHT;
                return true;
            }
            if (top) {
                *result = HTTOP;
                return true;
            }
            if (bottom) {
                *result = HTBOTTOM;
                return true;
            }
        }

        auto inWidget = [&](const QWidget* w) -> bool {
            if (w == nullptr || !w->isVisible()) {
                return false;
            }
            const QRect rect(w->mapToGlobal(QPoint(0, 0)), w->size());
            return rect.contains(globalPos);
        };

        if (inWidget(closeBtn_)) {
            *result = HTCLOSE;
            return true;
        }
        if (inWidget(maxBtn_)) {
            *result = HTMAXBUTTON;
            return true;
        }
        if (inWidget(minBtn_)) {
            *result = HTMINBUTTON;
            return true;
        }

        if (titleBar_ != nullptr) {
            const QRect titleRect(titleBar_->mapToGlobal(QPoint(0, 0)), titleBar_->size());
            if (titleRect.contains(globalPos)) {
                if (inWidget(viewProfileBtn_)) {
                    *result = HTCLIENT;
                    return true;
                }
                *result = HTCAPTION;
                return true;
            }
        }
    }
#else
    Q_UNUSED(eventType);
    Q_UNUSED(message);
    Q_UNUSED(result);
#endif
    return QMainWindow::nativeEvent(eventType, message, result);
}

bool ChatWindow::eventFilter(QObject* watched, QEvent* event) {
    if (watched == inputEdit_ && event->type() == QEvent::KeyPress) {
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        const int key = keyEvent->key();
        if ((key == Qt::Key_Return || key == Qt::Key_Enter) && (keyEvent->modifiers() & Qt::ShiftModifier) == 0) {
            onSendMessage();
            return true;
        }
    }

    return QMainWindow::eventFilter(watched, event);
}

void ChatWindow::setupUi() {
    setWindowTitle("LanTalk");
    setWindowIcon(makeAppIcon());
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    resize(1320, 820);

    auto* central = new QWidget(this);
    auto* mainLayout = new QVBoxLayout(central);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    auto* body = new QWidget(central);
    auto* bodyLayout = new QHBoxLayout(body);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(0);

    auto* rail = new QFrame(body);
    rail->setObjectName("Rail");
    rail->setFixedWidth(82);
    auto* railLayout = new QVBoxLayout(rail);
    railLayout->setContentsMargins(10, 12, 10, 12);
    railLayout->setSpacing(10);

    selfAvatarBtn_ = new QPushButton(rail);
    selfAvatarBtn_->setObjectName("AvatarBtn");
    selfAvatarBtn_->setFixedSize(56, 56);
    selfAvatarBtn_->setIconSize(QSize(56, 56));
    selfAvatarBtn_->setCursor(Qt::PointingHandCursor);
    selfAvatarBtn_->setToolTip("个人设置");

    settingsBtn_ = new QToolButton(rail);
    settingsBtn_->setObjectName("SettingsBtn");
    settingsBtn_->setText("设置");
    settingsBtn_->setCursor(Qt::PointingHandCursor);
    settingsBtn_->setToolButtonStyle(Qt::ToolButtonTextOnly);

    railLayout->addWidget(selfAvatarBtn_, 0, Qt::AlignHCenter | Qt::AlignTop);
    railLayout->addStretch(1);
    railLayout->addWidget(settingsBtn_, 0, Qt::AlignHCenter | Qt::AlignBottom);

    auto* contactsPane = new QFrame(body);
    contactsPane->setObjectName("ContactsPane");
    contactsPane->setFixedWidth(320);
    auto* leftLayout = new QVBoxLayout(contactsPane);
    leftLayout->setContentsMargins(12, 12, 12, 12);
    leftLayout->setSpacing(8);

    auto* sessionLabel = new QLabel("会话", contactsPane);
    sessionLabel->setObjectName("SectionTitle");

    contactList_ = new QListWidget(contactsPane);
    contactList_->setSpacing(4);
    contactList_->setUniformItemSizes(false);
    contactList_->setIconSize(QSize(30, 30));

    leftLayout->addWidget(sessionLabel);
    leftLayout->addWidget(contactList_, 1);

    auto* chatPane = new QFrame(body);
    chatPane->setObjectName("ChatPane");
    auto* chatRoot = new QVBoxLayout(chatPane);
    chatRoot->setContentsMargins(0, 0, 0, 0);
    chatRoot->setSpacing(0);

    titleBar_ = new QFrame(chatPane);
    titleBar_->setObjectName("TitleBar");
    titleBar_->setFixedHeight(72);
    auto* titleLayout = new QHBoxLayout(titleBar_);
    titleLayout->setContentsMargins(18, 8, 8, 7);
    titleLayout->setSpacing(10);

    chatTitleLabel_ = new QLabel("聊天窗口", titleBar_);
    chatTitleLabel_->setObjectName("ChatTitle");

    auto* titleRight = new QVBoxLayout();
    titleRight->setContentsMargins(0, 0, 0, 0);
    titleRight->setSpacing(4);

    auto* controlRow = new QHBoxLayout();
    controlRow->setContentsMargins(0, 0, 0, 0);
    controlRow->setSpacing(0);

    auto buildTitleBtn = [&](const QString& glyph) -> QPushButton* {
        auto* btn = new QPushButton(glyph, titleBar_);
        btn->setObjectName("TitleCtrlBtn");
        btn->setFixedSize(46, 30);
        btn->setCursor(Qt::ArrowCursor);
        QFont f("Segoe MDL2 Assets", 10);
        f.setStyleStrategy(QFont::PreferAntialias);
        btn->setFont(f);
        return btn;
    };
    minBtn_ = buildTitleBtn(QString(QChar(0xE921)));
    maxBtn_ = buildTitleBtn(QString(QChar(0xE922)));
    closeBtn_ = buildTitleBtn(QString(QChar(0xE8BB)));
    closeBtn_->setObjectName("TitleCloseBtn");

    viewProfileBtn_ = new QPushButton("查看资料", titleBar_);
    viewProfileBtn_->setObjectName("ProfileBtn");
    viewProfileBtn_->setCursor(Qt::PointingHandCursor);
    viewProfileBtn_->setFixedHeight(24);
    viewProfileBtn_->setEnabled(false);

    controlRow->addWidget(minBtn_);
    controlRow->addWidget(maxBtn_);
    controlRow->addWidget(closeBtn_);
    titleRight->addLayout(controlRow);
    titleRight->addWidget(viewProfileBtn_, 0, Qt::AlignRight);

    titleLayout->addWidget(chatTitleLabel_, 1, Qt::AlignVCenter);
    titleLayout->addLayout(titleRight);

    auto* chatContent = new QWidget(chatPane);
    auto* rightLayout = new QVBoxLayout(chatContent);
    rightLayout->setContentsMargins(14, 12, 14, 12);
    rightLayout->setSpacing(8);

    conversationView_ = new QTextBrowser(chatPane);
    conversationView_->setOpenExternalLinks(true);

    auto* composeRow = new QHBoxLayout();
    composeRow->setSpacing(8);

    inputEdit_ = new QTextEdit(chatPane);
    inputEdit_->setPlaceholderText("输入消息（Enter发送，Shift+Enter换行）");
    inputEdit_->setFixedHeight(96);

    auto* buttonCol = new QVBoxLayout();
    sendBtn_ = new QPushButton("发送", chatPane);
    sendBtn_->setObjectName("PrimaryBtn");
    sendFileBtn_ = new QPushButton("发送文件", chatPane);
    sendFileBtn_->setObjectName("SecondaryFlatBtn");
    buttonCol->addWidget(sendBtn_);
    buttonCol->addWidget(sendFileBtn_);
    buttonCol->addStretch(1);

    composeRow->addWidget(inputEdit_, 1);
    composeRow->addLayout(buttonCol);

    rightLayout->addWidget(conversationView_, 1);
    rightLayout->addLayout(composeRow);

    chatRoot->addWidget(titleBar_);
    chatRoot->addWidget(chatContent, 1);

    bodyLayout->addWidget(rail);
    bodyLayout->addWidget(contactsPane);
    bodyLayout->addWidget(chatPane, 1);

    mainLayout->addWidget(body, 1);

    setCentralWidget(central);

    inputEdit_->installEventFilter(this);

    setStyleSheet(R"(
        QMainWindow { background: #eef2f7; }
        QFrame#TitleBar {
            background: qlineargradient(x1:0,y1:0,x2:0,y2:1,stop:0 #f9fbff,stop:1 #f4f7fb);
            border-bottom: 1px solid #dde3eb;
        }
        QLabel#ChatTitle {
            color: #0b1220;
            font-size: 17px;
            font-weight: 700;
            padding-left: 2px;
        }
        QFrame#Rail {
            background: #f4f6fa;
            border-right: 1px solid #dde3eb;
        }
        QFrame#ContactsPane {
            background: #f8fafd;
            border-right: 1px solid #dde3eb;
        }
        QFrame#ChatPane {
            background: qlineargradient(x1:0,y1:0,x2:0,y2:1,stop:0 #ffffff, stop:1 #fafcff);
        }
        QLabel#SectionTitle {
            color: #111827;
            font-size: 14px;
            font-weight: 700;
            padding-left: 2px;
        }
        QListWidget {
            border: none;
            background: transparent;
            color: #111827;
            font-size: 13px;
        }
        QListWidget::item {
            border-radius: 0;
            border-bottom: 1px solid rgba(148, 163, 184, 0.16);
            padding: 10px 8px;
        }
        QListWidget::item:selected {
            background: rgba(59, 130, 246, 0.12);
            color: #0f172a;
        }
        QTextBrowser {
            border: none;
            border-bottom: 1px solid rgba(148, 163, 184, 0.25);
            background: transparent;
            color: #111827;
            font-size: 13px;
        }
        QTextEdit {
            border: none;
            border-radius: 0;
            background: transparent;
            color: #111827;
            font-size: 13px;
            padding: 4px 2px;
        }
        QPushButton {
            min-height: 34px;
            border: none;
            border-radius: 0;
            background: transparent;
            color: #111827;
            font-weight: 600;
            padding: 0 12px;
        }
        QPushButton:hover { background: rgba(0, 0, 0, 0.03); }
        QPushButton#PrimaryBtn {
            color: #2563eb;
            font-weight: 700;
        }
        QPushButton#PrimaryBtn:hover { color: #1d4ed8; background: rgba(37, 99, 235, 0.08); }
        QPushButton#PrimaryBtn:pressed { color: #1e3a8a; background: rgba(37, 99, 235, 0.14); }
        QPushButton#SecondaryFlatBtn {
            color: #475569;
            font-weight: 600;
        }
        QPushButton#SecondaryFlatBtn:hover { color: #334155; background: rgba(71, 85, 105, 0.08); }
        QPushButton#SecondaryFlatBtn:pressed { color: #1f2937; background: rgba(71, 85, 105, 0.14); }
        QPushButton#AvatarBtn {
            border: none;
            border-radius: 28px;
            background: transparent;
            padding: 0;
        }
        QPushButton#AvatarBtn:hover { background: rgba(0,0,0,0.04); }
        QToolButton#SettingsBtn {
            border: none;
            border-radius: 8px;
            color: #334155;
            font-size: 13px;
            font-weight: 700;
            min-width: 56px;
            min-height: 34px;
            background: rgba(15,23,42,0.06);
        }
        QToolButton#SettingsBtn:hover { background: rgba(15,23,42,0.12); }
        QPushButton#TitleCtrlBtn {
            border: none;
            border-radius: 0;
            background: transparent;
            color: #111111;
            font-family: "Segoe MDL2 Assets";
            font-size: 10px;
            font-weight: 400;
            min-height: 30px;
            min-width: 46px;
            padding: 0 0 1px 0;
        }
        QPushButton#TitleCtrlBtn:hover {
            background: rgba(0, 0, 0, 0.07);
        }
        QPushButton#TitleCtrlBtn:pressed {
            background: rgba(0, 0, 0, 0.12);
        }
        QPushButton#TitleCloseBtn {
            border: none;
            border-radius: 0;
            background: transparent;
            color: #111111;
            font-family: "Segoe MDL2 Assets";
            font-size: 10px;
            font-weight: 400;
            min-height: 30px;
            min-width: 46px;
            padding: 0 0 1px 0;
        }
        QPushButton#TitleCloseBtn:hover {
            background: #e81123;
            color: #ffffff;
        }
        QPushButton#TitleCloseBtn:pressed {
            background: #c42b1c;
            color: #ffffff;
        }
        QPushButton#ProfileBtn {
            border: 1px solid #d7e0ee;
            border-radius: 8px;
            background: #f6f9ff;
            color: #1d4ed8;
            font-size: 13px;
            font-weight: 600;
            padding: 0 10px;
        }
        QPushButton#ProfileBtn:hover {
            background: #e9f1ff;
            border-color: #b9d0f8;
            color: #1e40af;
        }
        QPushButton#ProfileBtn:disabled {
            color: #94a3b8;
            background: #f8fafc;
            border-color: #e2e8f0;
        }
    )");
}

void ChatWindow::bindEvents() {
    connect(selfAvatarBtn_, &QPushButton::clicked, this, [this]() { openSettingsDialog(); });
    connect(settingsBtn_, &QToolButton::clicked, this, [this]() { openSettingsDialog(); });
    connect(viewProfileBtn_, &QPushButton::clicked, this, [this]() { openContactProfileDialog(); });
    connect(sendBtn_, &QPushButton::clicked, this, [this]() { onSendMessage(); });
    connect(sendFileBtn_, &QPushButton::clicked, this, [this]() { onSendFile(); });
    connect(minBtn_, &QPushButton::clicked, this, [this]() {
        ::PostMessageW(reinterpret_cast<HWND>(winId()), WM_SYSCOMMAND, SC_MINIMIZE, 0);
    });
    connect(maxBtn_, &QPushButton::clicked, this, [this]() {
        if (isMaximized()) {
            ::PostMessageW(reinterpret_cast<HWND>(winId()), WM_SYSCOMMAND, SC_RESTORE, 0);
        } else {
            ::PostMessageW(reinterpret_cast<HWND>(winId()), WM_SYSCOMMAND, SC_MAXIMIZE, 0);
        }
    });
    connect(closeBtn_, &QPushButton::clicked, this, [this]() {
        ::PostMessageW(reinterpret_cast<HWND>(winId()), WM_SYSCOMMAND, SC_CLOSE, 0);
    });

    connect(contactList_, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem* current, QListWidgetItem*) {
                if (current == nullptr) {
                    activeContactId_.clear();
                    renderCurrentConversation();
                    updateChatHeader();
                    return;
                }
                activeContactId_ = current->data(Qt::UserRole).toString();
                if (Contact* contact = findContact(activeContactId_)) {
                    if (contact->unread != 0) {
                        contact->unread = 0;
                        saveContacts();
                    }
                }
                rebuildContactList();
                renderCurrentConversation();
                updateChatHeader();
            });
}

void ChatWindow::onSendMessage() {
    const QString userId = currentContactId();
    if (userId.isEmpty()) {
        QMessageBox::information(this, "提示", "请先选择联系人。");
        return;
    }

    const QString text = inputEdit_->toPlainText().trimmed();
    if (text.isEmpty()) {
        return;
    }

    std::string error;
    if (!app_.sendTextToUserId(userId.toStdString(), text.toStdString(), &error)) {
        QMessageBox::warning(this, "发送失败", QString::fromStdString(error.empty() ? "消息发送失败。" : error));
        return;
    }

    inputEdit_->clear();
}

void ChatWindow::onSendFile() {
    const QString userId = currentContactId();
    if (userId.isEmpty()) {
        QMessageBox::information(this, "提示", "请先选择联系人。");
        return;
    }

    const QString filePath = QFileDialog::getOpenFileName(this, "选择要发送的文件");
    if (filePath.isEmpty()) {
        return;
    }

    std::string error;
    if (!app_.sendFileToUserId(userId.toStdString(), fs::path(filePath.toStdString()), &error)) {
        QMessageBox::warning(this, "发送失败", QString::fromStdString(error.empty() ? "文件发送失败。" : error));
    }
}

void ChatWindow::openSettingsDialog() {
    Config config = app_.configCopy();
    QString selectedAvatar = selfAvatarPath_;

    QDialog dialog(this);
    dialog.setWindowTitle("个人设置");
    dialog.resize(430, 360);

    auto* root = new QVBoxLayout(&dialog);
    root->setContentsMargins(14, 14, 14, 14);
    root->setSpacing(12);

    auto* avatarRow = new QHBoxLayout();
    auto* avatarLabel = new QLabel(&dialog);
    avatarLabel->setFixedSize(72, 72);
    avatarLabel->setPixmap(makeRoundAvatar(QImage(selectedAvatar), 72, QString::fromStdString(config.userId)));

    auto* pickAvatarBtn = new QPushButton("上传头像", &dialog);
    avatarRow->addWidget(avatarLabel, 0, Qt::AlignLeft | Qt::AlignVCenter);
    avatarRow->addWidget(pickAvatarBtn, 0, Qt::AlignLeft | Qt::AlignVCenter);
    avatarRow->addStretch(1);

    auto* form = new QFormLayout();
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    form->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);

    auto* nicknameEdit = new QLineEdit(QString::fromStdString(config.userName), &dialog);
    form->addRow("昵称", nicknameEdit);

    auto* idLabel = new QLabel(QString::fromStdString(config.userId), &dialog);
    idLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow("我的用户ID", idLabel);

    auto* ipLabel = new QLabel(localIpSummary(), &dialog);
    ipLabel->setWordWrap(true);
    ipLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow("本机IP", ipLabel);

    auto* dataLabel = new QLabel(dataDirPath(), &dialog);
    dataLabel->setWordWrap(true);
    dataLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow("数据目录", dataLabel);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);

    connect(pickAvatarBtn, &QPushButton::clicked, &dialog, [&]() {
        const QString path = QFileDialog::getOpenFileName(&dialog,
                                                          "选择头像",
                                                          QFileInfo(selectedAvatar).absolutePath(),
                                                          "图片文件 (*.png *.jpg *.jpeg *.bmp *.webp)");
        if (path.isEmpty()) {
            return;
        }
        QImage image(path);
        if (image.isNull()) {
            QMessageBox::warning(&dialog, "提示", "头像文件无效，请重新选择。");
            return;
        }
        selectedAvatar = path;
        avatarLabel->setPixmap(makeRoundAvatar(image, 72, QString::fromStdString(config.userId)));
    });

    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    root->addLayout(avatarRow);
    root->addLayout(form);
    root->addStretch(1);
    root->addWidget(buttons);

    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    const QString newName = nicknameEdit->text().trimmed();
    if (newName.isEmpty()) {
        QMessageBox::warning(this, "提示", "昵称不能为空。");
        return;
    }

    std::string error;
    if (!app_.updateLocalUserName(newName.toStdString(), &error)) {
        QMessageBox::warning(this, "提示", QString::fromStdString(error.empty() ? "昵称保存失败。" : error));
        return;
    }

    selfAvatarPath_ = selectedAvatar;
    saveProfile();
    applySelfAvatar();
    syncLocalAvatarToNetwork();
}

void ChatWindow::openContactProfileDialog() {
    Contact* contact = findContact(activeContactId_);
    if (contact == nullptr) {
        QMessageBox::information(this, "提示", "请先选择联系人。");
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle("查看资料");
    dialog.resize(420, 340);

    auto* root = new QVBoxLayout(&dialog);
    root->setContentsMargins(14, 14, 14, 14);
    root->setSpacing(12);

    auto* avatarRow = new QHBoxLayout();
    auto* avatarLabel = new QLabel(&dialog);
    avatarLabel->setFixedSize(72, 72);
    avatarLabel->setPixmap(makeRoundAvatar(decodeAvatarPayload(contact->avatarPayload), 72, contact->name + contact->userId));

    auto* titleCol = new QVBoxLayout();
    auto* nameLabel = new QLabel(displayName(*contact), &dialog);
    nameLabel->setStyleSheet("font-size:16px;font-weight:700;color:#0f172a;");
    auto* stateLabel = new QLabel("状态  ●", &dialog);
    stateLabel->setStyleSheet(contact->online ? "color:#16a34a;font-size:12px;" : "color:#64748b;font-size:12px;");
    titleCol->addWidget(nameLabel);
    titleCol->addWidget(stateLabel);
    titleCol->addStretch(1);

    avatarRow->addWidget(avatarLabel, 0, Qt::AlignLeft | Qt::AlignVCenter);
    avatarRow->addLayout(titleCol, 1);

    auto* form = new QFormLayout();
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    form->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);

    auto* rawNameLabel = new QLabel(contact->name.isEmpty() ? contact->userId : contact->name, &dialog);
    rawNameLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow("用户名", rawNameLabel);

    auto* remarkEdit = new QLineEdit(contact->remark, &dialog);
    remarkEdit->setPlaceholderText("可填写备注，留空表示不设置");
    form->addRow("备注", remarkEdit);

    auto* idLabel = new QLabel(contact->userId, &dialog);
    idLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow("用户ID", idLabel);

    auto* ipLabel = new QLabel(contact->ip.isEmpty() ? "未知" : contact->ip, &dialog);
    ipLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow("IP", ipLabel);

    auto* bindTip = new QLabel("备注与用户ID绑定，同一用户会一直使用该备注。", &dialog);
    bindTip->setStyleSheet("color:#64748b;font-size:12px;");
    bindTip->setWordWrap(true);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    root->addLayout(avatarRow);
    root->addLayout(form);
    root->addWidget(bindTip);
    root->addStretch(1);
    root->addWidget(buttons);

    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    const QString newRemark = remarkEdit->text().trimmed();
    if (contact->remark == newRemark) {
        return;
    }

    contact->remark = newRemark;
    saveContacts();
    rebuildContactList();
    renderCurrentConversation();
    updateChatHeader();
}

void ChatWindow::refreshOnlinePeers() {
    const std::vector<Peer> peers = app_.snapshotPeers();
    bool changed = false;

    QSet<QString> onlineIds;
    for (const Peer& peer : peers) {
        const QString userId = QString::fromStdString(peer.userId);
        if (!userId.isEmpty()) {
            onlineIds.insert(userId);
        }
    }

    for (auto& contact : contacts_) {
        const bool shouldOnline = onlineIds.contains(contact.userId);
        if (contact.online != shouldOnline) {
            contact.online = shouldOnline;
            changed = true;
        }
    }

    const qint64 now = nowMs();
    for (const Peer& peer : peers) {
        const QString userId = QString::fromStdString(peer.userId);
        if (userId.isEmpty()) {
            continue;
        }

        Contact* contactPtr = findContact(userId);
        if (contactPtr == nullptr) {
            contactPtr = &ensureContact(userId);
            changed = true;
        }
        Contact& contact = *contactPtr;

        const QString newName = QString::fromStdString(peer.name).trimmed();
        const QString newIp = QString::fromStdString(peer.ip).trimmed();
        const QString newAvatarPayload = QString::fromStdString(peer.avatarPayload).trimmed();

        if (!newName.isEmpty() && contact.name != newName) {
            contact.name = newName;
            changed = true;
        }
        if (!newIp.isEmpty() && contact.ip != newIp) {
            contact.ip = newIp;
            changed = true;
        }
        if (contact.avatarPayload != newAvatarPayload) {
            contact.avatarPayload = newAvatarPayload;
            changed = true;
        }
        if (!contact.online) {
            contact.online = true;
            changed = true;
        }
        contact.lastSeenMs = now;
    }

    if (changed) {
        saveContacts();
        rebuildContactList();
        renderCurrentConversation();
        updateChatHeader();
    }
}

void ChatWindow::rebuildContactList() {
    std::sort(contacts_.begin(), contacts_.end(), [](const Contact& a, const Contact& b) {
        if (a.online != b.online) {
            return a.online > b.online;
        }
        const qint64 aTs = lastMessageTs(a);
        const qint64 bTs = lastMessageTs(b);
        if (aTs != bTs) {
            return aTs > bTs;
        }
        return a.name < b.name;
    });

    const QSignalBlocker blocker(contactList_);
    contactList_->clear();

    int activeRow = -1;
    for (int i = 0; i < static_cast<int>(contacts_.size()); ++i) {
        const Contact& contact = contacts_[static_cast<size_t>(i)];
        QString text = displayName(contact);
        if (contact.unread > 0) {
            text += QString("  [%1]").arg(contact.unread);
        }

        auto* item = new QListWidgetItem(makeContactAvatarIcon(contact.avatarPayload, contact.name + contact.userId, contact.online), text);
        item->setData(Qt::UserRole, contact.userId);

        QString toolTip = QString("用户ID: %1\nIP: %2").arg(contact.userId, contact.ip);
        if (!contact.remark.isEmpty()) {
            toolTip += QString("\n备注: %1").arg(contact.remark);
        }
        item->setToolTip(toolTip);
        contactList_->addItem(item);

        if (!activeContactId_.isEmpty() && contact.userId == activeContactId_) {
            activeRow = i;
        }
    }

    if (activeRow >= 0) {
        contactList_->setCurrentRow(activeRow);
    } else if (contactList_->count() > 0) {
        contactList_->setCurrentRow(0);
        activeContactId_ = contactList_->item(0)->data(Qt::UserRole).toString();
    } else {
        activeContactId_.clear();
    }
}

void ChatWindow::renderCurrentConversation() {
    const Contact* contact = findContact(activeContactId_);
    if (contact == nullptr) {
        conversationView_->setHtml(
            "<div style='color:#6b7280;font-size:14px;padding:18px;'>"
            "请选择左侧联系人开始聊天。"
            "</div>");
        return;
    }

    const QString peerTitle = displayName(*contact);

    QString html;
    html += "<html><body style='font-family:Microsoft YaHei UI;font-size:13px;background:transparent;padding:4px 2px;'>";
    for (const ChatMessage& message : contact->messages) {
        const QString sender = message.incoming ? peerTitle : QStringLiteral("我");
        const QString align = message.incoming ? "left" : "right";
        const QString accent = message.incoming ? "#c7d2e0" : "#93b4ff";
        const QString edgeStyle = message.incoming
                                      ? QString("border-left:2px solid %1;padding-left:10px;").arg(accent)
                                      : QString("border-right:2px solid %1;padding-right:10px;").arg(accent);

        QString content;
        if (message.isFile) {
            QString fileLabel = message.fileName;
            if (fileLabel.isEmpty()) {
                fileLabel = QFileInfo(message.filePath).fileName();
            }
            const QString link = QUrl::fromLocalFile(message.filePath).toString();
            content = QString("<a style='color:#1d4ed8;text-decoration:none;' href='%1'>文件：%2</a>")
                          .arg(link, htmlEscape(fileLabel));
        } else {
            content = htmlEscape(message.text).replace("\n", "<br/>");
        }

        html += QString(
                    "<div style='margin:10px 0;text-align:%1;'>"
                    "<div style='font-size:11px;color:#8b95a7;margin-bottom:3px;'>%2  %3</div>"
                    "<div style='display:inline-block;max-width:74%%;%4color:#0f172a;line-height:1.58;'>%5</div>"
                    "</div>")
                    .arg(align, htmlEscape(sender), timeText(message.timestampMs), edgeStyle, content);
    }
    html += "</body></html>";

    conversationView_->setHtml(html);
    if (QScrollBar* bar = conversationView_->verticalScrollBar()) {
        bar->setValue(bar->maximum());
    }
}

void ChatWindow::updateChatHeader() {
    const Contact* contact = findContact(activeContactId_);
    if (contact == nullptr) {
        chatTitleLabel_->setText("聊天窗口");
        viewProfileBtn_->setEnabled(false);
        return;
    }

    chatTitleLabel_->setText(displayName(*contact));
    viewProfileBtn_->setEnabled(true);
}

void ChatWindow::handleMessageEvent(const MessageEvent& event) {
    QString userId = QString::fromStdString(event.peerUserId).trimmed();
    if (userId.isEmpty()) {
        userId = QString("peer_%1")
                     .arg(QString::fromStdString(shortHashHex(event.peerName + "@" + event.peerIp)));
    }

    Contact& contact = ensureContact(userId);

    const QString peerName = QString::fromStdString(event.peerName).trimmed();
    const QString peerIp = QString::fromStdString(event.peerIp).trimmed();
    if (!peerName.isEmpty()) {
        contact.name = peerName;
    }
    if (!peerIp.isEmpty()) {
        contact.ip = peerIp;
    }
    contact.online = true;
    contact.lastSeenMs = nowMs();

    ChatMessage message;
    const qint64 fallbackTs = nowMs();
    message.timestampMs = (event.timestamp > 0) ? static_cast<qint64>(event.timestamp) * 1000 : fallbackTs;
    message.incoming = event.incoming;
    message.isFile = event.isFile;
    message.text = QString::fromStdString(event.text);
    message.fileName = QString::fromStdString(event.fileName);
    message.filePath = QString::fromStdString(event.filePath);

    contact.messages.push_back(message);

    if (activeContactId_ != userId) {
        contact.unread += 1;
    }

    saveHistory(contact);
    saveContacts();
    rebuildContactList();
    renderCurrentConversation();
    updateChatHeader();
}

ChatWindow::Contact* ChatWindow::findContact(const QString& userId) {
    for (auto& contact : contacts_) {
        if (contact.userId == userId) {
            return &contact;
        }
    }
    return nullptr;
}

const ChatWindow::Contact* ChatWindow::findContact(const QString& userId) const {
    for (const auto& contact : contacts_) {
        if (contact.userId == userId) {
            return &contact;
        }
    }
    return nullptr;
}

ChatWindow::Contact& ChatWindow::ensureContact(const QString& userId) {
    if (Contact* existing = findContact(userId)) {
        return *existing;
    }

    Contact contact;
    contact.userId = userId;
    contact.name = userId;
    contact.lastSeenMs = nowMs();
    contacts_.push_back(contact);
    return contacts_.back();
}

QString ChatWindow::currentContactId() const {
    if (!activeContactId_.isEmpty()) {
        return activeContactId_;
    }
    QListWidgetItem* item = contactList_->currentItem();
    if (item == nullptr) {
        return {};
    }
    return item->data(Qt::UserRole).toString();
}

void ChatWindow::loadHistory(Contact& contact) {
    contact.messages.clear();

    QFile file(historyFilePath(contact.userId));
    if (!file.exists()) {
        return;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        return;
    }

    const QByteArray decrypted = decryptBlob(file.readAll());
    const QJsonDocument doc = QJsonDocument::fromJson(decrypted);
    if (!doc.isArray()) {
        return;
    }

    const QJsonArray arr = doc.array();
    for (const QJsonValue& value : arr) {
        if (!value.isObject()) {
            continue;
        }
        const QJsonObject obj = value.toObject();

        ChatMessage msg;
        msg.timestampMs = static_cast<qint64>(obj.value("ts").toDouble());
        msg.incoming = obj.value("incoming").toBool();
        msg.isFile = obj.value("is_file").toBool();
        msg.text = obj.value("text").toString();
        msg.fileName = obj.value("file_name").toString();
        msg.filePath = obj.value("file_path").toString();
        contact.messages.push_back(msg);
    }
}

void ChatWindow::saveHistory(const Contact& contact) const {
    QDir().mkpath(chatsDirPath());

    QJsonArray arr;
    for (const ChatMessage& msg : contact.messages) {
        QJsonObject obj;
        obj.insert("ts", static_cast<double>(msg.timestampMs));
        obj.insert("incoming", msg.incoming);
        obj.insert("is_file", msg.isFile);
        obj.insert("text", msg.text);
        obj.insert("file_name", msg.fileName);
        obj.insert("file_path", msg.filePath);
        arr.push_back(obj);
    }

    QFile file(historyFilePath(contact.userId));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return;
    }

    const QByteArray plain = QJsonDocument(arr).toJson(QJsonDocument::Compact);
    file.write(encryptBlob(plain));
}

void ChatWindow::loadContacts() {
    QDir().mkpath(chatsDirPath());

    QFile file(contactFilePath());
    if (!file.exists()) {
        return;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        return;
    }

    const QByteArray decrypted = decryptBlob(file.readAll());
    const QJsonDocument doc = QJsonDocument::fromJson(decrypted);
    if (!doc.isArray()) {
        return;
    }

    contacts_.clear();
    const QJsonArray arr = doc.array();
    contacts_.reserve(static_cast<size_t>(arr.size()));

    for (const QJsonValue& value : arr) {
        if (!value.isObject()) {
            continue;
        }
        const QJsonObject obj = value.toObject();
        Contact contact;
        contact.userId = obj.value("user_id").toString().trimmed();
        if (contact.userId.isEmpty()) {
            continue;
        }
        contact.name = obj.value("name").toString(contact.userId);
        contact.remark = obj.value("remark").toString();
        contact.ip = obj.value("ip").toString();
        contact.avatarPayload = obj.value("avatar").toString();
        contact.online = false;
        contact.unread = obj.value("unread").toInt(0);
        contact.lastSeenMs = static_cast<qint64>(obj.value("last_seen").toDouble());
        loadHistory(contact);
        contacts_.push_back(std::move(contact));
    }

    rebuildContactList();
    renderCurrentConversation();
    updateChatHeader();
}

void ChatWindow::saveContacts() const {
    QDir().mkpath(dataDirPath());

    QJsonArray arr;
    for (const Contact& contact : contacts_) {
        QJsonObject obj;
        obj.insert("user_id", contact.userId);
        obj.insert("name", contact.name);
        obj.insert("remark", contact.remark);
        obj.insert("ip", contact.ip);
        obj.insert("avatar", contact.avatarPayload);
        obj.insert("unread", contact.unread);
        obj.insert("last_seen", static_cast<double>(contact.lastSeenMs));
        arr.push_back(obj);
    }

    QFile file(contactFilePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return;
    }

    const QByteArray plain = QJsonDocument(arr).toJson(QJsonDocument::Compact);
    file.write(encryptBlob(plain));
}

QString ChatWindow::dataDirPath() const {
    return QString::fromStdString(app_.dataDirString());
}

QString ChatWindow::chatsDirPath() const {
    return QDir(dataDirPath()).filePath("chats");
}

QString ChatWindow::contactFilePath() const {
    return QDir(dataDirPath()).filePath("contacts.dat");
}

QString ChatWindow::historyFilePath(const QString& userId) const {
    const QString token = safeFileToken(userId);
    return QDir(chatsDirPath()).filePath(token + ".dat");
}

QString ChatWindow::profileFilePath() const {
    return QDir(dataDirPath()).filePath("profile.json");
}

void ChatWindow::loadProfile() {
    QFile file(profileFilePath());
    if (!file.exists()) {
        return;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        return;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject()) {
        return;
    }
    const QJsonObject obj = doc.object();
    selfAvatarPath_ = obj.value("avatar_path").toString();
    if (!selfAvatarPath_.isEmpty() && QImage(selfAvatarPath_).isNull()) {
        selfAvatarPath_.clear();
    }
}

void ChatWindow::saveProfile() const {
    QDir().mkpath(dataDirPath());

    QJsonObject obj;
    obj.insert("avatar_path", selfAvatarPath_);

    QFile file(profileFilePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return;
    }
    file.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
}

void ChatWindow::ensureDefaultAvatarLibrary() {
    defaultAvatarPaths_.clear();
    const QStringList builtIns = builtInAvatarPaths();
    for (const QString& path : builtIns) {
        if (!QImage(path).isNull()) {
            defaultAvatarPaths_.push_back(path);
        }
    }
}

QByteArray ChatWindow::buildAvatarPayload(const QString& avatarPath) const {
    QImage image;
    if (!avatarPath.isEmpty()) {
        image.load(avatarPath);
    }
    const Config config = app_.configCopy();
    const QString seed = QString::fromStdString(config.userName + config.userId);
    if (image.isNull()) {
        image = makeDefaultAvatar(seed, 96).toImage();
    }

    const int size = 56;
    const QImage scaled = image.scaled(size, size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    if (scaled.isNull()) {
        return {};
    }

    QImage out(size, size, QImage::Format_RGB888);
    out.fill(Qt::white);
    {
        QPainter painter(&out);
        painter.setRenderHint(QPainter::Antialiasing, true);
        QPainterPath clip;
        clip.addEllipse(0, 0, size, size);
        painter.setClipPath(clip);
        painter.drawImage(0, 0, scaled);
    }

    QByteArray bytes;
    {
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::WriteOnly);
        if (!out.save(&buffer, "JPG", 68)) {
            return {};
        }
    }
    return bytes.toBase64();
}

void ChatWindow::syncLocalAvatarToNetwork() {
    std::string error;
    const QByteArray payload = buildAvatarPayload(selfAvatarPath_);
    if (!app_.updateLocalAvatarPayload(payload.toStdString(), &error)) {
        Q_UNUSED(error);
    }
}

void ChatWindow::applySelfAvatar() {
    const Config config = app_.configCopy();
    const QString seed = QString::fromStdString(config.userName + config.userId);

    QImage image;
    if (!selfAvatarPath_.isEmpty()) {
        image.load(selfAvatarPath_);
    }

    const QPixmap avatar = makeRoundAvatar(image, 56, seed);
    selfAvatarBtn_->setIcon(QIcon(avatar));
    selfAvatarBtn_->setIconSize(QSize(56, 56));
}

QString ChatWindow::localIpSummary() const {
    QStringList ips;
    for (const QHostAddress& addr : QNetworkInterface::allAddresses()) {
        if (addr.protocol() != QAbstractSocket::IPv4Protocol) {
            continue;
        }
        if (addr.isLoopback()) {
            continue;
        }
        ips.push_back(addr.toString());
    }
    ips.removeDuplicates();
    if (ips.isEmpty()) {
        return "未检测到";
    }
    return ips.join(" / ");
}

QString ChatWindow::displayName(const Contact& contact) const {
    if (!contact.remark.trimmed().isEmpty()) {
        return contact.remark.trimmed();
    }
    if (!contact.name.trimmed().isEmpty()) {
        return contact.name.trimmed();
    }
    return contact.userId;
}

uint64_t ChatWindow::storageSeed() const {
    return app_.localStorageKey() ^ 0x6A09E667F3BCC909ULL;
}

QByteArray ChatWindow::encryptBlob(const QByteArray& plain) const {
    const uint64_t nonce = QRandomGenerator::global()->generate64();

    QByteArray cipher = plain;
    CipherState cipherState = initCipherState(storageSeed() ^ nonce);
    if (!cipher.isEmpty()) {
        xorCipherInPlace(cipher.data(), static_cast<size_t>(cipher.size()), cipherState);
    }

    QByteArray out;
    out.reserve(4 + 8 + cipher.size());
    out.append(kBlobMagic, 4);
    out.append(nonceToBytes(nonce));
    out.append(cipher);
    return out;
}

QByteArray ChatWindow::decryptBlob(const QByteArray& blob) const {
    if (blob.size() >= 12 && std::memcmp(blob.constData(), kBlobMagic, 4) == 0) {
        const uint64_t nonce = bytesToNonce(blob, 4);
        QByteArray plain = blob.mid(12);
        CipherState cipherState = initCipherState(storageSeed() ^ nonce);
        if (!plain.isEmpty()) {
            xorCipherInPlace(plain.data(), static_cast<size_t>(plain.size()), cipherState);
        }
        return plain;
    }
    return QByteArray();
}
