#include "chat_window.h"

#include <QAbstractSocket>
#include <QApplication>
#include <QBuffer>
#include <QCloseEvent>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QEventLoop>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontMetrics>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHostAddress>
#include <QIcon>
#include <QImage>
#include <QGuiApplication>
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
#include <QNetworkAccessManager>
#include <QNetworkInterface>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPushButton>
#include <QRandomGenerator>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QSignalBlocker>
#include <QScreen>
#include <QShowEvent>
#include <QSplitter>
#include <QStringList>
#include <QTextEdit>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>
#include <QWidget>
#include <QCursor>
#include <QResizeEvent>

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

QString wrapTextByPixelWidth(const QString& value, const QFontMetrics& metrics, int maxWidthPx) {
    if (value.isEmpty() || maxWidthPx <= 0) {
        return value;
    }

    QStringList lines;
    const QStringList paragraphs = value.split('\n', Qt::KeepEmptyParts);
    for (const QString& paragraph : paragraphs) {
        if (paragraph.isEmpty()) {
            lines.push_back(QString());
            continue;
        }

        QString line;
        int lineWidth = 0;
        for (const QChar ch : paragraph) {
            const int chWidth = std::max(1, metrics.horizontalAdvance(ch));
            if (!line.isEmpty() && lineWidth + chWidth > maxWidthPx) {
                lines.push_back(line);
                line.clear();
                lineWidth = 0;
                if (ch.isSpace()) {
                    continue;
                }
            }

            line.push_back(ch);
            lineWidth += chWidth;
        }
        lines.push_back(line);
    }
    return lines.join('\n');
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
        return 0;
    }
    return contact.messages.back().timestampMs;
}

QPixmap makeDefaultAvatar(const QString& seed, int size) {
    QPixmap pix(size, size);
    pix.fill(Qt::transparent);

    const uint32_t hash = static_cast<uint32_t>(qHash(seed.isEmpty() ? QStringLiteral("L") : seed));
    const int hue = static_cast<int>(hash % 360U);
    const QColor top = QColor::fromHsl(hue, 170, 145);
    const QColor bottom = QColor::fromHsl((hue + 26) % 360, 150, 120);

    QPainter painter(&pix);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QRectF outerRect(1, 1, size - 2.0, size - 2.0);
    const qreal outerRadius = std::max<qreal>(8.0, size * 0.24);
    QLinearGradient background(0, 0, size, size);
    background.setColorAt(0.0, top);
    background.setColorAt(1.0, bottom);
    painter.setPen(Qt::NoPen);
    painter.setBrush(background);
    painter.drawRoundedRect(outerRect, outerRadius, outerRadius);

    painter.setPen(QPen(QColor(255, 255, 255, 105), std::max(1, size / 24)));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(outerRect.adjusted(1.0, 1.0, -1.0, -1.0), outerRadius - 1.0, outerRadius - 1.0);

    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(255, 255, 255, 46));
    painter.drawRoundedRect(QRectF(size * 0.14, size * 0.10, size * 0.58, size * 0.32), size * 0.14, size * 0.14);

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
    const qreal radius = std::max<qreal>(7.0, size * 0.22);
    path.addRoundedRect(QRectF(0, 0, size, size), radius, radius);
    painter.setClipPath(path);
    painter.drawImage(0, 0, scaled);
    return pix;
}

QIcon makeAppIcon() {
    const QIcon icon(":/app/app_icon.png");
    if (!icon.isNull()) {
        return icon;
    }
    return QIcon("app_icon.png");
}

constexpr char kBlobMagic[] = "LTC2";
constexpr char kDefaultSignalServer[] = "https://lantalk-web.pages.dev";

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
    QDir dir(":/avatars");
    QStringList filters;
    filters << "default_*.png";
    const QStringList names = dir.entryList(filters, QDir::Files, QDir::Name);

    QStringList out;
    out.reserve(names.size());
    for (const QString& name : names) {
        out.push_back(QString(":/avatars/%1").arg(name));
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

QIcon makeContactAvatarIcon(const QString& avatarPayload, const QString& fallbackSeed, const QColor& dotColor) {
    const QImage image = decodeAvatarPayload(avatarPayload);
    QPixmap base = makeRoundAvatar(image, 30, fallbackSeed);

    QPainter painter(&base);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QRectF dotRect(20.0, 20.0, 10.0, 10.0);
    painter.setPen(QPen(Qt::white, 1.2));
    painter.setBrush(dotColor);
    painter.drawEllipse(dotRect);

    // Keep avatar identical in selected/active states to avoid Qt palette tinting.
    QIcon icon;
    icon.addPixmap(base, QIcon::Normal, QIcon::Off);
    icon.addPixmap(base, QIcon::Normal, QIcon::On);
    icon.addPixmap(base, QIcon::Selected, QIcon::Off);
    icon.addPixmap(base, QIcon::Selected, QIcon::On);
    icon.addPixmap(base, QIcon::Active, QIcon::Off);
    icon.addPixmap(base, QIcon::Active, QIcon::On);
    return icon;
}

void clearLayout(QLayout* layout) {
    if (layout == nullptr) {
        return;
    }
    while (QLayoutItem* item = layout->takeAt(0)) {
        if (QLayout* child = item->layout()) {
            clearLayout(child);
            delete child;
        }
        if (QWidget* widget = item->widget()) {
            delete widget;
        }
        delete item;
    }
}
}  // namespace

ChatWindow::ChatWindow() {
    setupUi();
    bindEvents();
    signalingNet_ = new QNetworkAccessManager(this);

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
    if (signalingServers_.isEmpty()) {
        signalingServers_ = QStringList{QString::fromUtf8(kDefaultSignalServer)};
        saveProfile();
    }
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
    refreshSignalingPeers();
    pollSignalMessages();
    updateChatHeader();

    refreshTimer_ = new QTimer(this);
    refreshTimer_->setInterval(1000);
    connect(refreshTimer_, &QTimer::timeout, this, [this]() { refreshOnlinePeers(); });
    refreshTimer_->start();

    signalingTimer_ = new QTimer(this);
    signalingTimer_->setInterval(3000);
    connect(signalingTimer_, &QTimer::timeout, this, [this]() {
        refreshSignalingPeers();
        pollSignalMessages();
    });
    signalingTimer_->start();
}

ChatWindow::~ChatWindow() {
    if (signalingTimer_ != nullptr) {
        signalingTimer_->stop();
    }
    app_.setMessageCallback(nullptr);
    app_.setEventCallback(nullptr);
    app_.shutdown();
}

void ChatWindow::closeEvent(QCloseEvent* event) {
    app_.setMessageCallback(nullptr);
    app_.setEventCallback(nullptr);
    event->accept();
}

void ChatWindow::changeEvent(QEvent* event) {
    if (event != nullptr && event->type() == QEvent::WindowStateChange && maxBtn_ != nullptr) {
        maxBtn_->setText(isMaximized() ? QString(QChar(0xE923)) : QString(QChar(0xE922)));
        refreshWindowBorder();
    }
    QMainWindow::changeEvent(event);
}

void ChatWindow::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
    refreshWindowBorder();
    renderCurrentConversation();
}

void ChatWindow::showEvent(QShowEvent* event) {
    QMainWindow::showEvent(event);
    refreshWindowBorder();
}

bool ChatWindow::nativeEvent(const QByteArray& eventType, void* message, qintptr* result) {
#ifdef Q_OS_WIN
    Q_UNUSED(eventType);
    MSG* msg = static_cast<MSG*>(message);
    if (msg != nullptr && msg->message == WM_NCHITTEST) {
        const QPoint globalPos = QCursor::pos();
        const QRect winRect = frameGeometry();
        const int border = 8;

        auto inWidget = [&](const QWidget* w) -> bool {
            if (w == nullptr || !w->isVisible()) {
                return false;
            }
            const QRect rect(w->mapToGlobal(QPoint(0, 0)), w->size());
            return rect.contains(globalPos);
        };

        if (inWidget(closeBtn_) || inWidget(maxBtn_) || inWidget(minBtn_)) {
            // Keep title buttons in client area so Qt click handlers always work.
            *result = HTCLIENT;
            return true;
        }

        if (!isMaximized()) {
            const bool left = globalPos.x() >= winRect.left() && globalPos.x() < winRect.left() + border;
            const bool right = globalPos.x() <= winRect.right() && globalPos.x() > winRect.right() - border;
            const bool top = globalPos.y() >= winRect.top() && globalPos.y() < winRect.top() + border;
            const bool bottom = globalPos.y() <= winRect.bottom() && globalPos.y() > winRect.bottom() - border;
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

        if (titleBar_ != nullptr) {
            const QRect titleRect(titleBar_->mapToGlobal(QPoint(0, 0)), titleBar_->size());
            if (titleRect.contains(globalPos)) {
                if (inWidget(viewProfileBtn_)) {
                    *result = HTCLIENT;
                    return true;
                }
                const int firstRowBottom = titleRect.top() + 34;
                if (globalPos.y() <= firstRowBottom) {
                    *result = HTCAPTION;
                    return true;
                }
                *result = HTCLIENT;
                return true;
            }
        }

        if (inWidget(contactsTopDrag_)) {
            *result = HTCAPTION;
            return true;
        }

        if (searchEdit_ != nullptr && searchEdit_->parentWidget() != nullptr) {
            QWidget* pane = searchEdit_->parentWidget();
            const QRect paneRect(pane->mapToGlobal(QPoint(0, 0)), pane->size());
            if (paneRect.contains(globalPos) && globalPos.y() <= paneRect.top() + 34) {
                *result = HTCAPTION;
                return true;
            }
        }

        if (inWidget(railPane_) && !inWidget(settingsBtn_)) {
            *result = HTCAPTION;
            return true;
        }

        if (inWidget(searchEdit_) || inWidget(contactList_) || inWidget(settingsBtn_) || inWidget(inputEdit_) ||
            inWidget(conversationView_) || inWidget(sendBtn_) || inWidget(sendFileBtn_) || inWidget(emojiBtn_)) {
            *result = HTCLIENT;
            return true;
        }

        if (centralWidget() != nullptr) {
            const QRect clientRect(centralWidget()->mapToGlobal(QPoint(0, 0)), centralWidget()->size());
            if (clientRect.contains(globalPos)) {
                *result = HTCLIENT;
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
    railPane_ = rail;
    rail->setFixedWidth(76);
    auto* railLayout = new QVBoxLayout(rail);
    railLayout->setContentsMargins(10, 10, 10, 10);
    railLayout->setSpacing(10);

    selfAvatarBtn_ = new QPushButton(rail);
    selfAvatarBtn_->setObjectName("AvatarBtn");
    selfAvatarBtn_->setFixedSize(50, 50);
    selfAvatarBtn_->setIconSize(QSize(50, 50));
    selfAvatarBtn_->setCursor(Qt::ArrowCursor);
    selfAvatarBtn_->setFocusPolicy(Qt::NoFocus);
    selfAvatarBtn_->setAttribute(Qt::WA_TransparentForMouseEvents, true);

    settingsBtn_ = new QToolButton(rail);
    settingsBtn_->setObjectName("SettingsBtn");
    settingsBtn_->setText(QString(QChar(0xE713)));
    QFont settingsFont("Segoe MDL2 Assets", 15);
    settingsFont.setStyleStrategy(QFont::PreferAntialias);
    settingsBtn_->setFont(settingsFont);
    settingsBtn_->setToolTip("设置");
    settingsBtn_->setCursor(Qt::PointingHandCursor);
    settingsBtn_->setToolButtonStyle(Qt::ToolButtonTextOnly);

    railLayout->addSpacing(12);
    railLayout->addWidget(selfAvatarBtn_, 0, Qt::AlignHCenter | Qt::AlignTop);
    railLayout->addStretch(1);
    railLayout->addWidget(settingsBtn_, 0, Qt::AlignHCenter | Qt::AlignBottom);
    railLayout->addSpacing(4);

    auto* contactsPane = new QFrame(body);
    contactsPane->setObjectName("ContactsPane");
    contactsPane->setMinimumWidth(210);
    contactsPane->setMaximumWidth(520);
    auto* leftLayout = new QVBoxLayout(contactsPane);
    leftLayout->setContentsMargins(12, 0, 12, 12);
    leftLayout->setSpacing(8);

    contactsTopDrag_ = new QWidget(contactsPane);
    contactsTopDrag_->setObjectName("ContactsTopDrag");
    contactsTopDrag_->setFixedHeight(30);
    contactsTopDrag_->setCursor(Qt::ArrowCursor);

    searchEdit_ = new QLineEdit(contactsPane);
    searchEdit_->setObjectName("SearchBox");
    searchEdit_->setPlaceholderText("搜索联系人");
    searchEdit_->setClearButtonEnabled(true);
    searchEdit_->setFixedHeight(34);

    contactList_ = new QListWidget(contactsPane);
    contactList_->setSpacing(4);
    contactList_->setUniformItemSizes(false);
    contactList_->setIconSize(QSize(30, 30));
    contactList_->setFocusPolicy(Qt::NoFocus);

    leftLayout->addWidget(contactsTopDrag_);
    leftLayout->addSpacing(4);
    leftLayout->addWidget(searchEdit_);
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
    titleLayout->setContentsMargins(18, 0, 0, 5);
    titleLayout->setSpacing(10);

    chatTitleLabel_ = new QLabel("聊天窗口", titleBar_);
    chatTitleLabel_->setObjectName("ChatTitle");

    auto* titleRight = new QVBoxLayout();
    titleRight->setContentsMargins(0, 0, 0, 0);
    titleRight->setSpacing(0);

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

    viewProfileBtn_ = new QPushButton("···", titleBar_);
    viewProfileBtn_->setObjectName("ProfileBtn");
    QFont profileFont;
    profileFont.setPointSize(13);
    profileFont.setWeight(QFont::DemiBold);
    profileFont.setStyleStrategy(QFont::PreferAntialias);
    viewProfileBtn_->setFont(profileFont);
    viewProfileBtn_->setCursor(Qt::PointingHandCursor);
    viewProfileBtn_->setToolTip("查看资料");
    viewProfileBtn_->setFixedSize(30, 24);
    viewProfileBtn_->setEnabled(false);

    controlRow->addWidget(minBtn_);
    controlRow->addWidget(maxBtn_);
    controlRow->addWidget(closeBtn_);
    titleRight->addLayout(controlRow);
    titleRight->addSpacing(4);
    auto* profileRow = new QHBoxLayout();
    profileRow->setContentsMargins(0, 0, 0, 0);
    profileRow->setSpacing(0);
    profileRow->addStretch(1);
    profileRow->addWidget(viewProfileBtn_, 0, Qt::AlignVCenter);
    profileRow->addSpacing(6);
    titleRight->addLayout(profileRow);

    titleLayout->addWidget(chatTitleLabel_, 1, Qt::AlignVCenter);
    titleLayout->addLayout(titleRight);
    titleLayout->setAlignment(titleRight, Qt::AlignTop | Qt::AlignRight);

    auto* chatContent = new QWidget(chatPane);
    auto* rightLayout = new QVBoxLayout(chatContent);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(0);

    conversationView_ = new QScrollArea(chatContent);
    conversationView_->setObjectName("ConversationView");
    conversationView_->setFrameShape(QFrame::NoFrame);
    conversationView_->setWidgetResizable(true);
    conversationView_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    conversationView_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    conversationList_ = new QWidget(conversationView_);
    conversationList_->setObjectName("ConversationList");
    conversationLayout_ = new QVBoxLayout(conversationList_);
    conversationLayout_->setContentsMargins(14, 12, 14, 14);
    conversationLayout_->setSpacing(0);
    conversationLayout_->addStretch(1);
    conversationView_->setWidget(conversationList_);

    auto* composeArea = new QWidget(chatContent);
    composeArea->setObjectName("ComposeArea");
    auto* composeLayout = new QVBoxLayout(composeArea);
    composeLayout->setContentsMargins(12, 8, 12, 12);
    composeLayout->setSpacing(6);

    auto* toolsRow = new QHBoxLayout();
    toolsRow->setContentsMargins(0, 0, 0, 0);
    toolsRow->setSpacing(4);

    emojiBtn_ = new QToolButton(composeArea);
    emojiBtn_->setObjectName("ComposeIconBtn");
    emojiBtn_->setText(QString(QChar(0xE11D)));
    QFont emojiFont("Segoe MDL2 Assets", 14);
    emojiFont.setStyleStrategy(QFont::PreferAntialias);
    emojiBtn_->setFont(emojiFont);
    emojiBtn_->setCursor(Qt::PointingHandCursor);
    emojiBtn_->setToolTip("表情");
    emojiBtn_->setFixedSize(38, 34);

    sendFileBtn_ = new QPushButton(QString(QChar(0xE898)), composeArea);
    sendFileBtn_->setObjectName("ComposeIconBtn");
    QFont fileFont("Segoe MDL2 Assets", 13);
    fileFont.setStyleStrategy(QFont::PreferAntialias);
    sendFileBtn_->setFont(fileFont);
    sendFileBtn_->setCursor(Qt::PointingHandCursor);
    sendFileBtn_->setToolTip("发送文件");
    sendFileBtn_->setFixedSize(38, 34);

    toolsRow->addWidget(emojiBtn_);
    toolsRow->addWidget(sendFileBtn_);
    toolsRow->addStretch(1);

    auto* inputRow = new QHBoxLayout();
    inputRow->setContentsMargins(0, 0, 0, 0);
    inputRow->setSpacing(8);

    inputEdit_ = new QTextEdit(chatContent);
    inputEdit_->setPlaceholderText("输入消息（Enter发送，Shift+Enter换行）");
    inputEdit_->setMinimumHeight(72);
    inputEdit_->setAcceptRichText(false);
    inputEdit_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    inputEdit_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    sendBtn_ = new QPushButton("发送", chatContent);
    sendBtn_->setObjectName("PrimaryBtn");
    sendBtn_->setFixedWidth(82);
    sendBtn_->setFixedHeight(34);

    inputRow->addWidget(inputEdit_, 1);
    inputRow->addWidget(sendBtn_, 0, Qt::AlignBottom);

    composeLayout->addLayout(toolsRow, 0);
    composeLayout->addLayout(inputRow, 1);

    auto* chatSplitter = new QSplitter(Qt::Vertical, chatContent);
    chatSplitter->setObjectName("ChatSplitter");
    chatSplitter->setChildrenCollapsible(false);
    chatSplitter->setHandleWidth(1);
    chatSplitter->addWidget(conversationView_);
    chatSplitter->addWidget(composeArea);
    chatSplitter->setStretchFactor(0, 1);
    chatSplitter->setStretchFactor(1, 0);
    chatSplitter->setSizes(QList<int>{570, 170});
    connect(chatSplitter, &QSplitter::splitterMoved, this, [this](int, int) { renderCurrentConversation(); });

    rightLayout->addWidget(chatSplitter, 1);

    chatRoot->addWidget(titleBar_);
    chatRoot->addWidget(chatContent, 1);

    auto* leftComposite = new QWidget(body);
    auto* leftCompositeLayout = new QHBoxLayout(leftComposite);
    leftCompositeLayout->setContentsMargins(0, 0, 0, 0);
    leftCompositeLayout->setSpacing(0);
    leftCompositeLayout->addWidget(rail);
    leftCompositeLayout->addWidget(contactsPane, 1);
    leftComposite->setMinimumWidth(rail->width() + contactsPane->minimumWidth());
    leftComposite->setMaximumWidth(rail->width() + contactsPane->maximumWidth());

    auto* mainSplitter = new QSplitter(Qt::Horizontal, body);
    mainSplitter->setObjectName("MainSplitter");
    mainSplitter->setChildrenCollapsible(false);
    mainSplitter->setHandleWidth(1);
    mainSplitter->addWidget(leftComposite);
    mainSplitter->addWidget(chatPane);
    mainSplitter->setStretchFactor(0, 0);
    mainSplitter->setStretchFactor(1, 1);
    mainSplitter->setSizes(QList<int>{368, 952});
    connect(mainSplitter, &QSplitter::splitterMoved, this, [this](int, int) { renderCurrentConversation(); });

    bodyLayout->addWidget(mainSplitter, 1);

    mainLayout->addWidget(body, 1);

    setCentralWidget(central);
    windowBorder_ = new QFrame(this);
    windowBorder_->setObjectName("WindowBorder");
    windowBorder_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    windowBorder_->setFocusPolicy(Qt::NoFocus);
    refreshWindowBorder();

    inputEdit_->installEventFilter(this);

    setStyleSheet(R"(
        QMainWindow { background: #dcdfe5; }
        QFrame#Rail {
            background: #e7eaef;
            border-right: 1px solid #d3d8e0;
        }
        QFrame#ContactsPane {
            background: #f3f5f8;
            border-right: 1px solid #d9dee6;
        }
        QFrame#ChatPane {
            background: #f8f9fb;
        }
        QFrame#TitleBar {
            background: #f8f9fb;
            border-bottom: 1px solid #dde2e9;
        }
        QFrame#WindowBorder {
            border: 1px solid #bec6d2;
            background: transparent;
        }
        QLabel#ChatTitle {
            color: #14181f;
            font-size: 17px;
            font-weight: 700;
            padding-left: 2px;
        }
        QLineEdit#SearchBox {
            border: 1px solid #d7dce4;
            border-radius: 8px;
            background: #eef1f5;
            color: #1e2430;
            font-size: 13px;
            padding: 0 10px;
        }
        QLineEdit#SearchBox:focus {
            border-color: #bcc6d3;
            background: #ffffff;
        }
        QWidget#ContactsTopDrag {
            background: transparent;
        }
        QSplitter#MainSplitter::handle {
            background: #d7dce4;
        }
        QSplitter#MainSplitter::handle:hover {
            background: #c3cad5;
        }
        QSplitter#ChatSplitter::handle {
            background: #dfe4eb;
        }
        QSplitter#ChatSplitter::handle:hover {
            background: #cfd7e2;
        }
        QListWidget {
            border: none;
            background: transparent;
            color: #1e2430;
            font-size: 13px;
        }
        QListWidget::item {
            border: none;
            border-radius: 8px;
            padding: 10px 8px;
            margin: 1px 2px;
            outline: none;
        }
        QListWidget::item:hover { background: rgba(0, 0, 0, 0.03); }
        QListWidget::item:focus { outline: none; }
        QListWidget::item:selected {
            background: rgba(164, 173, 184, 0.14);
            color: #1e2430;
        }
        QScrollArea#ConversationView {
            border: none;
            border-bottom: 1px solid #dfe4eb;
            background: transparent;
        }
        QWidget#ConversationList {
            background: transparent;
        }
        QLabel#MessageMeta {
            color: #8b95a7;
            font-size: 11px;
        }
        QFrame#BubbleIncoming {
            background: #ffffff;
            border: 1px solid #e2e8f0;
            border-radius: 12px;
        }
        QFrame#BubbleOutgoing {
            background: #dff6e7;
            border: 1px solid #b7e7c8;
            border-radius: 12px;
        }
        QLabel#BubbleText {
            color: #0f172a;
            font-size: 13px;
            line-height: 1.5;
        }
        QWidget#ComposeArea {
            border-top: none;
            background: transparent;
        }
        QTextEdit {
            border: none;
            border-radius: 0;
            background: transparent;
            color: #1f2937;
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
        QPushButton:hover { background: rgba(0, 0, 0, 0.04); }
        QPushButton#PrimaryBtn {
            border-radius: 6px;
            background: #07c160;
            color: #ffffff;
            font-weight: 700;
            font-size: 11px;
            padding: 0 14px;
        }
        QPushButton#PrimaryBtn:hover { background: #06b35a; }
        QPushButton#PrimaryBtn:pressed { background: #059d4f; }
        QPushButton#ComposeIconBtn,
        QToolButton#ComposeIconBtn {
            min-width: 38px;
            max-width: 38px;
            min-height: 34px;
            max-height: 34px;
            border: none;
            border-radius: 8px;
            color: #4b5563;
            background: transparent;
            font-family: "Segoe MDL2 Assets";
            font-size: 16px;
            padding: 0;
        }
        QPushButton#ComposeIconBtn:hover,
        QToolButton#ComposeIconBtn:hover { background: rgba(0, 0, 0, 0.07); }
        QPushButton#ComposeIconBtn:pressed,
        QToolButton#ComposeIconBtn:pressed { background: rgba(0, 0, 0, 0.12); }
        QPushButton#AvatarBtn {
            border: none;
            border-radius: 12px;
            background: transparent;
            padding: 0;
        }
        QToolButton#SettingsBtn {
            border: none;
            border-radius: 10px;
            color: #4b5563;
            min-width: 38px;
            min-height: 34px;
            background: transparent;
        }
        QToolButton#SettingsBtn:hover { background: rgba(0, 0, 0, 0.07); }
        QToolButton#SettingsBtn:pressed { background: rgba(0, 0, 0, 0.12); }
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
        QPushButton#TitleCtrlBtn:hover { background: rgba(0, 0, 0, 0.07); }
        QPushButton#TitleCtrlBtn:pressed { background: rgba(0, 0, 0, 0.12); }
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
        QPushButton#TitleCloseBtn:hover { background: #e81123; color: #ffffff; }
        QPushButton#TitleCloseBtn:pressed { background: #c42b1c; color: #ffffff; }
        QPushButton#ProfileBtn {
            border: none;
            border-radius: 6px;
            background: transparent;
            color: #374151;
            min-width: 30px;
            min-height: 24px;
            font-size: 16px;
            font-weight: 700;
            padding: 0;
        }
        QPushButton#ProfileBtn:hover {
            background: rgba(0, 0, 0, 0.07);
            color: #111827;
        }
        QPushButton#ProfileBtn:pressed {
            background: rgba(0, 0, 0, 0.12);
            color: #111827;
        }
        QPushButton#ProfileBtn:disabled {
            color: #a8afb8;
            background: transparent;
        }
    )");
}

void ChatWindow::refreshWindowBorder() {
    if (windowBorder_ == nullptr) {
        return;
    }
    windowBorder_->setGeometry(rect());
    windowBorder_->setVisible(!isMaximized());
    windowBorder_->raise();
}

void ChatWindow::bindEvents() {
    connect(settingsBtn_, &QToolButton::clicked, this, [this]() { openSettingsDialog(); });
    connect(viewProfileBtn_, &QPushButton::clicked, this, [this]() { openContactProfileDialog(); });
    connect(emojiBtn_, &QToolButton::clicked, this, [this]() { openEmojiMenu(); });
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

    connect(searchEdit_, &QLineEdit::textChanged, this, [this]() {
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
        Contact* contact = findContact(userId);
        if (contact == nullptr || contact->signalServer.isEmpty() || contact->presence == PresenceKind::Offline) {
            QMessageBox::warning(this, "发送失败", QString::fromStdString(error.empty() ? "消息发送失败。" : error));
            return;
        }

        const Config cfg = app_.configCopy();
        const qint64 now = nowMs();
        const QString postUrl = signalApiUrl(contact->signalServer, "/v1/messages/send");
        QJsonObject req;
        req.insert("fromUserId", QString::fromStdString(cfg.userId));
        req.insert("fromName", QString::fromStdString(cfg.userName));
        req.insert("fromAvatar", QString::fromStdString(cfg.avatarPayload));
        req.insert("toUserId", contact->userId);
        req.insert("text", text);
        req.insert("timestampMs", static_cast<double>(now));

        QJsonDocument respDoc;
        QString signalErr;
        if (!signalRequest(postUrl, "POST", &req, &respDoc, &signalErr)) {
            const QString baseError = QString::fromStdString(error.empty() ? "消息发送失败。" : error);
            QMessageBox::warning(this, "发送失败", baseError + "\n信令兜底失败：" + signalErr);
            return;
        }
        appendSignalOutgoingMessage(*contact, text, now);
    }

    inputEdit_->clear();
}

void ChatWindow::onSendFile() {
    const QString userId = currentContactId();
    if (userId.isEmpty()) {
        QMessageBox::information(this, "提示", "请先选择联系人。");
        return;
    }

    Contact* contact = findContact(userId);
    if (contact != nullptr) {
        if (contact->presence == PresenceKind::SignalWs) {
            QMessageBox::information(this, "提示", "当前联系人使用WS兜底通道，暂不支持发送文件。");
            return;
        }
        if (contact->presence == PresenceKind::Offline) {
            QMessageBox::information(this, "提示", "联系人当前离线。");
            return;
        }
    }

    const QString filePath = QFileDialog::getOpenFileName(this, "选择要发送的文件");
    if (filePath.isEmpty()) {
        return;
    }

    std::string error;
    const fs::path nativePath(filePath.toStdWString());
    if (!app_.sendFileToUserId(userId.toStdString(), nativePath, &error)) {
        if (contact != nullptr && contact->presence == PresenceKind::SignalP2P) {
            QMessageBox::information(this, "提示", "P2P连接失败，暂不支持发送文件。");
            return;
        }
        QMessageBox::warning(this, "发送失败", QString::fromStdString(error.empty() ? "文件发送失败。" : error));
    }
}

void ChatWindow::openEmojiMenu() {
    if (inputEdit_ == nullptr || emojiBtn_ == nullptr) {
        return;
    }

    auto* popup = new QDialog(this, Qt::Popup | Qt::FramelessWindowHint);
    popup->setAttribute(Qt::WA_DeleteOnClose, true);
    popup->setObjectName("EmojiPopup");
    popup->setStyleSheet(R"(
        QDialog#EmojiPopup {
            background: #ffffff;
            border: 1px solid #d8dde5;
            border-radius: 10px;
        }
        QLabel#EmojiTitle {
            color: #111827;
            font-size: 12px;
            font-weight: 700;
        }
        QLabel#EmojiHint {
            color: #6b7280;
            font-size: 11px;
        }
        QToolButton#EmojiCell {
            border: none;
            border-radius: 8px;
            font-size: 18px;
            background: transparent;
            min-width: 34px;
            min-height: 34px;
            max-width: 34px;
            max-height: 34px;
        }
        QToolButton#EmojiCell:hover { background: #eef2f7; }
        QToolButton#EmojiCell:pressed { background: #e3e8ef; }
    )");

    auto* root = new QVBoxLayout(popup);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(8);

    auto* header = new QHBoxLayout();
    header->setContentsMargins(0, 0, 0, 0);
    header->setSpacing(6);

    auto* title = new QLabel("常用表情", popup);
    title->setObjectName("EmojiTitle");
    auto* hint = new QLabel("点击即可插入", popup);
    hint->setObjectName("EmojiHint");
    header->addWidget(title);
    header->addStretch(1);
    header->addWidget(hint);
    root->addLayout(header);

    auto* gridHost = new QWidget(popup);
    auto* grid = new QGridLayout(gridHost);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(4);
    grid->setVerticalSpacing(4);

    const QStringList emojis = {
        "😀", "😁", "😂", "🤣", "😅", "😊", "🙂", "😉",
        "😍", "😘", "😎", "🤔", "🤗", "🙃", "😴", "😭",
        "😤", "😡", "🤯", "🥳", "👍", "👎", "👏", "🙏",
        "👋", "🤝", "💪", "🎉", "🎁", "✨", "🔥", "❤️",
        "💯", "🌟", "✅", "❗", "❓", "😮", "🙌", "🤩"};
    constexpr int kCols = 8;
    for (int i = 0; i < emojis.size(); ++i) {
        const QString emoji = emojis.at(i);
        auto* btn = new QToolButton(gridHost);
        btn->setObjectName("EmojiCell");
        btn->setText(emoji);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setToolButtonStyle(Qt::ToolButtonTextOnly);
        connect(btn, &QToolButton::clicked, this, [this, popup, emoji]() {
            inputEdit_->insertPlainText(emoji);
            inputEdit_->setFocus();
            popup->close();
        });
        const int row = i / kCols;
        const int col = i % kCols;
        grid->addWidget(btn, row, col);
    }
    root->addWidget(gridHost);
    popup->adjustSize();

    QPoint pos = emojiBtn_->mapToGlobal(QPoint(0, emojiBtn_->height() + 6));
    if (QScreen* screen = QGuiApplication::screenAt(pos)) {
        const QRect bounds = screen->availableGeometry();
        if (pos.x() + popup->width() > bounds.right()) {
            pos.setX(bounds.right() - popup->width());
        }
        if (pos.y() + popup->height() > bounds.bottom()) {
            pos.setY(emojiBtn_->mapToGlobal(QPoint(0, -popup->height() - 6)).y());
        }
        if (pos.x() < bounds.left()) {
            pos.setX(bounds.left());
        }
        if (pos.y() < bounds.top()) {
            pos.setY(bounds.top());
        }
    }
    popup->move(pos);
    popup->show();
}

void ChatWindow::openSettingsDialog() {
    Config config = app_.configCopy();
    QString selectedAvatar = selfAvatarPath_;

    QDialog dialog(this);
    dialog.setWindowTitle("个人设置");
    dialog.resize(470, 390);
    QFont dialogFont = font();
    if (dialogFont.pointSize() < 11) {
        dialogFont.setPointSize(11);
    }
    dialogFont.setHintingPreference(QFont::PreferFullHinting);
    dialog.setFont(dialogFont);
    dialog.setStyleSheet(R"(
        QLabel { font-size: 14px; }
        QLineEdit {
            min-height: 30px;
            font-size: 14px;
            padding: 0 8px;
        }
        QTextEdit {
            min-height: 86px;
            font-size: 13px;
            border: 1px solid #cbd5e1;
            border-radius: 8px;
            padding: 6px 8px;
            background: #ffffff;
        }
        QPushButton {
            min-height: 32px;
            font-size: 14px;
            padding: 0 12px;
        }
    )");

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

    auto* signalEdit = new QTextEdit(&dialog);
    signalEdit->setAcceptRichText(false);
    signalEdit->setPlaceholderText("一行一个");
    signalEdit->setMinimumHeight(90);
    signalEdit->setPlainText(signalingServers_.join("\n"));
    form->addRow("信令服务器", signalEdit);

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
    signalingServers_ = normalizeSignalServers(signalEdit->toPlainText().split('\n'));
    saveProfile();
    applySelfAvatar();
    syncLocalAvatarToNetwork();
    refreshSignalingPeers();
    pollSignalMessages();
}

void ChatWindow::openContactProfileDialog() {
    Contact* contact = findContact(activeContactId_);
    if (contact == nullptr) {
        QMessageBox::information(this, "提示", "请先选择联系人。");
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle("查看资料");
    dialog.resize(450, 372);
    QFont dialogFont = font();
    if (dialogFont.pointSize() < 11) {
        dialogFont.setPointSize(11);
    }
    dialogFont.setHintingPreference(QFont::PreferFullHinting);
    dialog.setFont(dialogFont);
    dialog.setStyleSheet(R"(
        QLabel { font-size: 14px; }
        QLineEdit {
            min-height: 30px;
            font-size: 14px;
            padding: 0 8px;
        }
        QPushButton {
            min-height: 32px;
            font-size: 14px;
            padding: 0 12px;
        }
    )");

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
    auto* stateLabel = new QLabel(QString("状态  %1").arg(presenceText(contact->presence)), &dialog);
    QString stateColor = "#64748b";
    if (contact->presence == PresenceKind::Lan) {
        stateColor = "#16a34a";
    } else if (contact->presence == PresenceKind::SignalP2P) {
        stateColor = "#2563eb";
    } else if (contact->presence == PresenceKind::SignalWs) {
        stateColor = "#d97706";
    }
    stateLabel->setStyleSheet(QString("color:%1;font-size:13px;").arg(stateColor));
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
    bindTip->setStyleSheet("color:#64748b;font-size:13px;");
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
        if (contact.lanOnline != shouldOnline) {
            contact.lanOnline = shouldOnline;
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
        if (!contact.lanOnline) {
            contact.lanOnline = true;
            changed = true;
        }
        contact.lastSeenMs = now;
    }

    syncSignalPresence(&changed);

    // Keep only offline contacts that have at least one chat message.
    const size_t beforeCount = contacts_.size();
    contacts_.erase(std::remove_if(contacts_.begin(), contacts_.end(),
                                   [](const Contact& contact) { return !contact.online && contact.messages.empty(); }),
                    contacts_.end());
    if (contacts_.size() != beforeCount) {
        if (!activeContactId_.isEmpty() && findContact(activeContactId_) == nullptr) {
            activeContactId_.clear();
        }
        changed = true;
    }

    if (changed) {
        saveContacts();
        rebuildContactList();
        renderCurrentConversation();
        updateChatHeader();
    }
}

void ChatWindow::refreshSignalingPeers() {
    const Config cfg = app_.configCopy();
    const QString selfId = QString::fromStdString(cfg.userId).trimmed();
    if (selfId.isEmpty()) {
        return;
    }

    QSet<QString> p2pUsers;
    QSet<QString> wsUsers;
    QHash<QString, QString> serverByUserId;
    bool changed = false;

    if (!signalingServers_.isEmpty()) {
        QJsonArray localIps;
        for (const QHostAddress& addr : QNetworkInterface::allAddresses()) {
            if (addr.protocol() != QAbstractSocket::IPv4Protocol || addr.isLoopback()) {
                continue;
            }
            localIps.push_back(addr.toString());
        }

        for (const QString& server : signalingServers_) {
            QJsonObject presenceReq;
            presenceReq.insert("userId", selfId);
            presenceReq.insert("name", QString::fromStdString(cfg.userName));
            presenceReq.insert("avatarPayload", QString::fromStdString(cfg.avatarPayload));
            presenceReq.insert("listenPort", static_cast<int>(cfg.listenPort));
            presenceReq.insert("e2eePublic", QString::number(cfg.e2eePublic));
            presenceReq.insert("localIps", localIps);
            QJsonDocument ignored;
            QString ignoredErr;
            signalRequest(signalApiUrl(server, "/v1/presence"), "POST", &presenceReq, &ignored, &ignoredErr);

            QUrl peersUrl(signalApiUrl(server, "/v1/peers"));
            QUrlQuery query;
            query.addQueryItem("userId", selfId);
            peersUrl.setQuery(query);

            QJsonDocument peersDoc;
            QString peersErr;
            if (!signalRequest(peersUrl.toString(), "GET", nullptr, &peersDoc, &peersErr)) {
                continue;
            }
            if (!peersDoc.isObject()) {
                continue;
            }
            const QJsonArray peers = peersDoc.object().value("peers").toArray();
            for (const QJsonValue& peerValue : peers) {
                if (!peerValue.isObject()) {
                    continue;
                }
                const QJsonObject peerObj = peerValue.toObject();
                const QString userId = peerObj.value("userId").toString().trimmed();
                if (userId.isEmpty() || userId == selfId) {
                    continue;
                }

                const QString mode = peerObj.value("mode").toString("ws").toLower();
                const bool isP2P = (mode == "p2p");
                if (isP2P) {
                    p2pUsers.insert(userId);
                    wsUsers.remove(userId);
                    serverByUserId[userId] = server;
                } else if (!p2pUsers.contains(userId)) {
                    wsUsers.insert(userId);
                    if (!serverByUserId.contains(userId)) {
                        serverByUserId[userId] = server;
                    }
                }

                const bool isNewContact = (findContact(userId) == nullptr);
                Contact& contact = ensureContact(userId);
                if (isNewContact) {
                    changed = true;
                }
                const QString name = peerObj.value("name").toString().trimmed();
                const QString ip = peerObj.value("ip").toString().trimmed();
                const QString avatar = peerObj.value("avatarPayload").toString().trimmed();
                if (!name.isEmpty() && contact.name != name) {
                    contact.name = name;
                    changed = true;
                }
                if (!ip.isEmpty() && contact.ip != ip) {
                    contact.ip = ip;
                    changed = true;
                }
                if (!avatar.isEmpty() && contact.avatarPayload != avatar) {
                    contact.avatarPayload = avatar;
                    changed = true;
                }
                contact.lastSeenMs = nowMs();

                if (isP2P) {
                    bool okPort = false;
                    const int port = peerObj.value("port").toInt(0);
                    const uint16_t safePort = (port > 0 && port <= 65535) ? static_cast<uint16_t>(port) : 0;
                    okPort = (safePort > 0);
                    bool okKey = false;
                    const QString pubRaw = peerObj.value("e2eePublic").toVariant().toString();
                    const uint64_t pubKey = pubRaw.toULongLong(&okKey);
                    if (okPort && okKey && !ip.isEmpty()) {
                        app_.upsertSignalPeer(userId.toStdString(),
                                              contact.name.toStdString(),
                                              ip.toStdString(),
                                              safePort,
                                              pubKey,
                                              contact.avatarPayload.toStdString());
                    }
                }
            }
        }
    }

    if (signalP2PUsers_ != p2pUsers) {
        signalP2PUsers_ = p2pUsers;
        changed = true;
    }
    if (signalWsUsers_ != wsUsers) {
        signalWsUsers_ = wsUsers;
        changed = true;
    }
    if (signalServerByUserId_ != serverByUserId) {
        signalServerByUserId_ = serverByUserId;
        changed = true;
    }

    syncSignalPresence(&changed);
    if (changed) {
        saveContacts();
        rebuildContactList();
        renderCurrentConversation();
        updateChatHeader();
    }
}

void ChatWindow::pollSignalMessages() {
    const Config cfg = app_.configCopy();
    const QString selfId = QString::fromStdString(cfg.userId).trimmed();
    if (selfId.isEmpty() || signalingServers_.isEmpty()) {
        return;
    }

    bool changed = false;
    for (const QString& server : signalingServers_) {
        const qint64 after = signalAfterByServer_.value(server, 0);
        QUrl pullUrl(signalApiUrl(server, "/v1/messages/pull"));
        QUrlQuery query;
        query.addQueryItem("userId", selfId);
        query.addQueryItem("after", QString::number(after));
        pullUrl.setQuery(query);

        QJsonDocument doc;
        QString err;
        if (!signalRequest(pullUrl.toString(), "GET", nullptr, &doc, &err) || !doc.isObject()) {
            continue;
        }
        const QJsonArray messages = doc.object().value("messages").toArray();
        qint64 maxTs = after;
        for (const QJsonValue& value : messages) {
            if (!value.isObject()) {
                continue;
            }
            const QJsonObject obj = value.toObject();
            const QString id = obj.value("id").toString();
            if (!id.isEmpty()) {
                const QString key = server + "|" + id;
                if (seenSignalMsgIds_.contains(key)) {
                    continue;
                }
                seenSignalMsgIds_.insert(key);
            }

            const QString fromId = obj.value("fromUserId").toString().trimmed();
            const QString text = obj.value("text").toString();
            if (fromId.isEmpty() || fromId == selfId || text.isEmpty()) {
                continue;
            }

            const qint64 ts = static_cast<qint64>(obj.value("timestampMs").toDouble(static_cast<double>(nowMs())));
            if (ts > maxTs) {
                maxTs = ts;
            }

            Contact& contact = ensureContact(fromId);
            const QString fromName = obj.value("fromName").toString().trimmed();
            const QString avatar = obj.value("fromAvatar").toString().trimmed();
            if (!fromName.isEmpty() && contact.name != fromName) {
                contact.name = fromName;
            }
            if (!avatar.isEmpty() && contact.avatarPayload != avatar) {
                contact.avatarPayload = avatar;
            }
            contact.signalServer = server;
            contact.signalMode = "ws";
            contact.lastSeenMs = nowMs();
            signalWsUsers_.insert(fromId);
            signalP2PUsers_.remove(fromId);
            signalServerByUserId_[fromId] = server;

            ChatMessage message;
            message.timestampMs = ts;
            message.incoming = true;
            message.isFile = false;
            message.text = text;
            contact.messages.push_back(message);
            if (activeContactId_ != fromId) {
                contact.unread += 1;
            }
            saveHistory(contact);
            changed = true;
        }
        signalAfterByServer_[server] = maxTs;
    }

    syncSignalPresence(&changed);
    if (changed) {
        saveContacts();
        rebuildContactList();
        renderCurrentConversation();
        updateChatHeader();
    }
}

void ChatWindow::syncSignalPresence(bool* changed) {
    if (changed == nullptr) {
        return;
    }
    for (Contact& contact : contacts_) {
        const bool signalP2P = signalP2PUsers_.contains(contact.userId);
        const bool signalWs = signalWsUsers_.contains(contact.userId);
        const bool signalOnline = signalP2P || signalWs;

        PresenceKind nextPresence = PresenceKind::Offline;
        if (contact.lanOnline) {
            nextPresence = PresenceKind::Lan;
        } else if (signalP2P) {
            nextPresence = PresenceKind::SignalP2P;
        } else if (signalWs) {
            nextPresence = PresenceKind::SignalWs;
        }
        const bool nextOnline = nextPresence != PresenceKind::Offline;

        if (contact.signalOnline != signalOnline) {
            contact.signalOnline = signalOnline;
            *changed = true;
        }
        if (contact.presence != nextPresence) {
            contact.presence = nextPresence;
            *changed = true;
        }
        if (contact.online != nextOnline) {
            contact.online = nextOnline;
            *changed = true;
        }

        const QString nextServer = signalOnline ? signalServerByUserId_.value(contact.userId) : QString();
        const QString nextMode = signalP2P ? QStringLiteral("p2p") : (signalWs ? QStringLiteral("ws") : QString());
        if (contact.signalServer != nextServer) {
            contact.signalServer = nextServer;
            *changed = true;
        }
        if (contact.signalMode != nextMode) {
            contact.signalMode = nextMode;
            *changed = true;
        }
        if (nextOnline) {
            contact.lastSeenMs = std::max(contact.lastSeenMs, nowMs());
        }
    }
}

void ChatWindow::appendSignalOutgoingMessage(Contact& contact, const QString& text, qint64 timestampMs) {
    ChatMessage message;
    message.timestampMs = timestampMs;
    message.incoming = false;
    message.isFile = false;
    message.text = text;
    contact.messages.push_back(message);
    contact.lastSeenMs = timestampMs;
    saveHistory(contact);
    saveContacts();
    rebuildContactList();
    renderCurrentConversation();
    updateChatHeader();
}

void ChatWindow::rebuildContactList() {
    std::sort(contacts_.begin(), contacts_.end(), [](const Contact& a, const Contact& b) {
        const bool aHasChat = !a.messages.empty();
        const bool bHasChat = !b.messages.empty();
        if (aHasChat != bHasChat) {
            return aHasChat > bHasChat;
        }

        if (aHasChat) {
            const qint64 aTs = lastMessageTs(a);
            const qint64 bTs = lastMessageTs(b);
            if (aTs != bTs) {
                return aTs > bTs;
            }
        } else if (a.lastSeenMs != b.lastSeenMs) {
            return a.lastSeenMs > b.lastSeenMs;
        }

        if (a.online != b.online) {
            return a.online > b.online;
        }
        return a.name < b.name;
    });

    const QSignalBlocker blocker(contactList_);
    contactList_->clear();

    const QString keyword = (searchEdit_ == nullptr) ? QString() : searchEdit_->text().trimmed();
    int activeRow = -1;
    for (int i = 0; i < static_cast<int>(contacts_.size()); ++i) {
        const Contact& contact = contacts_[static_cast<size_t>(i)];
        const QString shownName = displayName(contact);
        if (!keyword.isEmpty()) {
            const QString haystack = (shownName + "\n" + contact.name + "\n" + contact.remark + "\n" + contact.userId);
            if (!haystack.contains(keyword, Qt::CaseInsensitive)) {
                continue;
            }
        }
        QString text = shownName;
        if (contact.unread > 0) {
            text += QString("  [%1]").arg(contact.unread);
        }

        QColor dotColor(148, 163, 184);
        if (contact.presence == PresenceKind::Lan) {
            dotColor = QColor(34, 197, 94);
        } else if (contact.presence == PresenceKind::SignalP2P) {
            dotColor = QColor(59, 130, 246);
        } else if (contact.presence == PresenceKind::SignalWs) {
            dotColor = QColor(245, 158, 11);
        }
        auto* item = new QListWidgetItem(makeContactAvatarIcon(contact.avatarPayload, contact.name + contact.userId, dotColor), text);
        item->setData(Qt::UserRole, contact.userId);

        QString toolTip = QString("用户ID: %1\nIP: %2\n状态: %3")
                              .arg(contact.userId,
                                   contact.ip.isEmpty() ? QStringLiteral("未知") : contact.ip,
                                   presenceText(contact.presence));
        if (!contact.signalServer.isEmpty()) {
            toolTip += QString("\n信令: %1").arg(contact.signalServer);
        }
        if (!contact.remark.isEmpty()) {
            toolTip += QString("\n备注: %1").arg(contact.remark);
        }
        item->setToolTip(toolTip);
        const int insertedRow = contactList_->count();
        contactList_->addItem(item);

        if (!activeContactId_.isEmpty() && contact.userId == activeContactId_) {
            activeRow = insertedRow;
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
    clearLayout(conversationLayout_);

    if (contact == nullptr) {
        auto* placeholder = new QLabel("请选择联系人开始聊天。", conversationList_);
        placeholder->setStyleSheet("color:#6b7280;font-size:16px;padding:26px 20px;");
        placeholder->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        conversationLayout_->addWidget(placeholder);
        conversationLayout_->addStretch(1);
        return;
    }

    const QString peerTitle = displayName(*contact);
    const QPixmap incomingAvatar =
        makeRoundAvatar(decodeAvatarPayload(contact->avatarPayload), 34, contact->name + contact->userId);
    const Config selfConfig = app_.configCopy();
    const QImage selfImage = decodeAvatarPayload(QString::fromStdString(selfConfig.avatarPayload));
    const QPixmap outgoingAvatar =
        makeRoundAvatar(selfImage, 34, QStringLiteral("self_") + QString::fromStdString(selfConfig.userId));

    const int viewWidth = (conversationView_ != nullptr && conversationView_->viewport() != nullptr)
                              ? conversationView_->viewport()->width()
                              : width();
    const int bubbleMaxWidth = std::max(180, ((std::max(420, viewWidth) - 112) * 2) / 3);
    const int bubbleContentMaxWidth = std::max(150, bubbleMaxWidth - 20);
    const QFontMetrics bubbleMetrics(conversationView_->font());

    for (const ChatMessage& message : contact->messages) {
        auto* rowWidget = new QWidget(conversationList_);
        auto* rowLayout = new QVBoxLayout(rowWidget);
        rowLayout->setContentsMargins(0, 0, 0, 8);
        rowLayout->setSpacing(3);

        const QString sender = message.incoming ? peerTitle : QStringLiteral("我");
        auto* metaLabel = new QLabel(QString("%1  %2").arg(sender, timeText(message.timestampMs)), rowWidget);
        metaLabel->setObjectName("MessageMeta");
        metaLabel->setAlignment(message.incoming ? (Qt::AlignLeft | Qt::AlignVCenter) : (Qt::AlignRight | Qt::AlignVCenter));
        if (message.incoming) {
            metaLabel->setContentsMargins(42, 0, 0, 0);
        } else {
            metaLabel->setContentsMargins(0, 0, 42, 0);
        }
        rowLayout->addWidget(metaLabel);

        auto* contentRow = new QWidget(rowWidget);
        auto* contentLayout = new QHBoxLayout(contentRow);
        contentLayout->setContentsMargins(0, 0, 0, 0);
        contentLayout->setSpacing(8);

        auto* avatarLabel = new QLabel(contentRow);
        avatarLabel->setFixedSize(34, 34);
        avatarLabel->setPixmap(message.incoming ? incomingAvatar : outgoingAvatar);
        avatarLabel->setScaledContents(true);

        auto* bubbleFrame = new QFrame(contentRow);
        bubbleFrame->setObjectName(message.incoming ? "BubbleIncoming" : "BubbleOutgoing");
        bubbleFrame->setMaximumWidth(bubbleMaxWidth);
        bubbleFrame->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);

        auto* bubbleLayout = new QVBoxLayout(bubbleFrame);
        bubbleLayout->setContentsMargins(10, 8, 10, 8);
        bubbleLayout->setSpacing(0);

        auto* bubbleLabel = new QLabel(bubbleFrame);
        bubbleLabel->setObjectName("BubbleText");
        bubbleLabel->setWordWrap(true);
        bubbleLabel->setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::TextSelectableByMouse);
        bubbleLabel->setMaximumWidth(bubbleContentMaxWidth);

        if (message.isFile) {
            QString fileLabel = message.fileName;
            if (fileLabel.isEmpty()) {
                fileLabel = QFileInfo(message.filePath).fileName();
            }
            fileLabel = wrapTextByPixelWidth(fileLabel, bubbleMetrics, bubbleContentMaxWidth);
            QString labelHtml = htmlEscape(fileLabel);
            labelHtml.replace("\n", "<br/>");
            const QString link = QUrl::fromLocalFile(message.filePath).toString();
            bubbleLabel->setTextFormat(Qt::RichText);
            bubbleLabel->setOpenExternalLinks(true);
            bubbleLabel->setText(
                QString("<a style='color:#1d4ed8;text-decoration:none;' href='%1'>文件：%2</a>").arg(link, labelHtml));
        } else {
            const QString wrappedText = wrapTextByPixelWidth(message.text, bubbleMetrics, bubbleContentMaxWidth);
            bubbleLabel->setTextFormat(Qt::PlainText);
            bubbleLabel->setText(wrappedText);
        }
        bubbleLayout->addWidget(bubbleLabel);

        if (message.incoming) {
            contentLayout->addWidget(avatarLabel, 0, Qt::AlignTop);
            contentLayout->addWidget(bubbleFrame, 0, Qt::AlignTop);
            contentLayout->addStretch(1);
        } else {
            contentLayout->addStretch(1);
            contentLayout->addWidget(bubbleFrame, 0, Qt::AlignTop);
            contentLayout->addWidget(avatarLabel, 0, Qt::AlignTop);
        }

        rowLayout->addWidget(contentRow);
        conversationLayout_->addWidget(rowWidget);
    }

    conversationLayout_->addStretch(1);

    auto scrollToBottom = [this]() {
        if (conversationView_ == nullptr) {
            return;
        }
        if (QScrollBar* bar = conversationView_->verticalScrollBar()) {
            bar->setValue(bar->maximum());
        }
    };
    scrollToBottom();
    QTimer::singleShot(0, this, scrollToBottom);
    QTimer::singleShot(30, this, scrollToBottom);
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
    const qint64 now = nowMs();
    contact.lanOnline = true;
    contact.lastSeenMs = now;
    bool presenceChanged = false;
    syncSignalPresence(&presenceChanged);

    ChatMessage message;
    const qint64 eventTs = (event.timestamp > 0) ? static_cast<qint64>(event.timestamp) * 1000 : 0;
    message.timestampMs = std::max(now, eventTs);
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
        contact.lanOnline = false;
        contact.signalOnline = false;
        contact.presence = PresenceKind::Offline;
        contact.signalMode.clear();
        contact.signalServer.clear();
        contact.unread = obj.value("unread").toInt(0);
        contact.lastSeenMs = static_cast<qint64>(obj.value("last_seen").toDouble());
        loadHistory(contact);
        contacts_.push_back(std::move(contact));
    }

    contacts_.erase(std::remove_if(contacts_.begin(), contacts_.end(),
                                   [](const Contact& contact) { return !contact.online && contact.messages.empty(); }),
                    contacts_.end());

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
    QStringList servers;
    const QJsonValue serversVal = obj.value("signaling_servers");
    if (serversVal.isArray()) {
        const QJsonArray arr = serversVal.toArray();
        for (const QJsonValue& v : arr) {
            if (v.isString()) {
                servers.push_back(v.toString());
            }
        }
    } else if (serversVal.isString()) {
        servers = serversVal.toString().split('\n');
    }
    signalingServers_ = normalizeSignalServers(servers);
}

void ChatWindow::saveProfile() const {
    QDir().mkpath(dataDirPath());

    QJsonObject obj;
    obj.insert("avatar_path", selfAvatarPath_);
    QJsonArray arr;
    for (const QString& server : signalingServers_) {
        arr.push_back(server);
    }
    obj.insert("signaling_servers", arr);

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
        clip.addRoundedRect(QRectF(0, 0, size, size), 14.0, 14.0);
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

QStringList ChatWindow::normalizeSignalServers(const QStringList& rawServers) const {
    QStringList out;
    QSet<QString> seen;
    for (QString item : rawServers) {
        item = item.trimmed();
        if (item.isEmpty()) {
            continue;
        }
        if (!item.contains("://")) {
            item = "https://" + item;
        }
        while (item.endsWith('/')) {
            item.chop(1);
        }
        const QUrl url(item);
        if (!url.isValid() || url.host().isEmpty()) {
            continue;
        }
        const QString normalized = url.toString(QUrl::RemovePath | QUrl::RemoveQuery | QUrl::RemoveFragment | QUrl::RemoveUserInfo);
        if (normalized.isEmpty() || seen.contains(normalized)) {
            continue;
        }
        seen.insert(normalized);
        out.push_back(normalized);
    }
    return out;
}

QString ChatWindow::signalApiUrl(const QString& serverBase, const QString& path) const {
    QString base = serverBase.trimmed();
    QString cleanPath = path.trimmed();
    if (!cleanPath.startsWith('/')) {
        cleanPath.prepend('/');
    }
    while (base.endsWith('/')) {
        base.chop(1);
    }
    if (base.isEmpty()) {
        return cleanPath;
    }

    QUrl baseUrl(base);
    const QString host = baseUrl.host().toLower();
    if (host.endsWith(".pages.dev")) {
        return base + "/api" + cleanPath;
    }
    if (base.endsWith("/api")) {
        return base + cleanPath;
    }
    return base + cleanPath;
}

bool ChatWindow::signalRequest(const QString& url,
                               const QString& method,
                               const QJsonObject* body,
                               QJsonDocument* outDoc,
                               QString* errorText) const {
    if (signalingNet_ == nullptr) {
        if (errorText != nullptr) {
            *errorText = "信令网络未初始化";
        }
        return false;
    }

    QNetworkRequest request{QUrl(url)};
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QNetworkReply* reply = nullptr;
    const QString methodUpper = method.trimmed().toUpper();
    if (methodUpper == "POST") {
        const QByteArray payload = body == nullptr ? QByteArray("{}") : QJsonDocument(*body).toJson(QJsonDocument::Compact);
        reply = signalingNet_->post(request, payload);
    } else {
        reply = signalingNet_->get(request);
    }
    if (reply == nullptr) {
        if (errorText != nullptr) {
            *errorText = "请求创建失败";
        }
        return false;
    }

    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeout.start(3000);
    loop.exec();

    if (!reply->isFinished()) {
        reply->abort();
        reply->deleteLater();
        if (errorText != nullptr) {
            *errorText = "请求超时";
        }
        return false;
    }

    if (reply->error() != QNetworkReply::NoError) {
        if (errorText != nullptr) {
            *errorText = reply->errorString();
        }
        reply->deleteLater();
        return false;
    }

    const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray bytes = reply->readAll();
    reply->deleteLater();
    if (statusCode >= 400) {
        if (errorText != nullptr) {
            *errorText = QString("HTTP %1").arg(statusCode);
        }
        return false;
    }

    if (outDoc != nullptr) {
        if (bytes.trimmed().isEmpty()) {
            *outDoc = QJsonDocument(QJsonObject{});
        } else {
            QJsonParseError parseError;
            const QJsonDocument doc = QJsonDocument::fromJson(bytes, &parseError);
            if (parseError.error != QJsonParseError::NoError) {
                if (errorText != nullptr) {
                    *errorText = "JSON解析失败";
                }
                return false;
            }
            *outDoc = doc;
        }
    }
    return true;
}

void ChatWindow::applySelfAvatar() {
    const Config config = app_.configCopy();
    const QString seed = QString::fromStdString(config.userName + config.userId);

    QImage image;
    if (!selfAvatarPath_.isEmpty()) {
        image.load(selfAvatarPath_);
    }

    const QPixmap avatar = makeRoundAvatar(image, 50, seed);
    selfAvatarBtn_->setIcon(QIcon(avatar));
    selfAvatarBtn_->setIconSize(QSize(50, 50));
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

QString ChatWindow::presenceText(PresenceKind kind) const {
    switch (kind) {
        case PresenceKind::Lan:
            return "局域网在线";
        case PresenceKind::SignalP2P:
            return "打洞在线";
        case PresenceKind::SignalWs:
            return "WS兜底在线";
        case PresenceKind::Offline:
        default:
            return "离线";
    }
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
