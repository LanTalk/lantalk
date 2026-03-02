#include "chat_window.h"

#include <QCloseEvent>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QMetaObject>
#include <QPainter>
#include <QPushButton>
#include <QScrollBar>
#include <QSet>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QTextBrowser>
#include <QTextEdit>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace {
QIcon statusIcon(bool online) {
    QPixmap pix(12, 12);
    pix.fill(Qt::transparent);

    QPainter painter(&pix);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(online ? QColor(34, 197, 94) : QColor(148, 163, 184));
    painter.drawEllipse(1, 1, 10, 10);
    return QIcon(pix);
}

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
}  // namespace

ChatWindow::ChatWindow() {
    setupUi();
    bindEvents();
    loadContacts();

    app_.setEventCallback([this](const std::string& line) {
        const QString text = QString::fromStdString(line);
        QMetaObject::invokeMethod(
            this,
            [this, text]() {
                statusBar()->showMessage(text, 2500);
            },
            Qt::QueuedConnection);
    });

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

    updateStatusBar();
    refreshOnlinePeers();

    refreshTimer_ = new QTimer(this);
    refreshTimer_->setInterval(1000);
    connect(refreshTimer_, &QTimer::timeout, this, [this]() {
        refreshOnlinePeers();
        updateStatusBar();
    });
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

void ChatWindow::setupUi() {
    setWindowTitle("LanTalk 局域网聊天");
    resize(1280, 800);

    auto* central = new QWidget(this);
    auto* root = new QHBoxLayout(central);
    root->setContentsMargins(14, 14, 14, 14);
    root->setSpacing(14);

    auto* leftCard = new QFrame(central);
    leftCard->setObjectName("Card");
    auto* leftLayout = new QVBoxLayout(leftCard);
    leftLayout->setContentsMargins(14, 14, 14, 14);
    leftLayout->setSpacing(10);

    auto* title = new QLabel("会话", leftCard);
    title->setObjectName("Title");

    contactList_ = new QListWidget(leftCard);
    contactList_->setSpacing(6);
    contactList_->setUniformItemSizes(false);

    leftLayout->addWidget(title);
    leftLayout->addWidget(contactList_, 1);

    auto* rightCard = new QFrame(central);
    rightCard->setObjectName("Card");
    auto* rightLayout = new QVBoxLayout(rightCard);
    rightLayout->setContentsMargins(16, 16, 16, 16);
    rightLayout->setSpacing(10);

    auto* topRow = new QHBoxLayout();
    auto* nameLabel = new QLabel("我的昵称", rightCard);
    nameLabel->setObjectName("Label");
    nameEdit_ = new QLineEdit(rightCard);
    nameEdit_->setPlaceholderText("输入昵称");
    renameBtn_ = new QPushButton("保存昵称", rightCard);
    renameBtn_->setObjectName("PrimaryBtn");

    topRow->addWidget(nameLabel);
    topRow->addWidget(nameEdit_, 1);
    topRow->addWidget(renameBtn_);

    conversationView_ = new QTextBrowser(rightCard);
    conversationView_->setOpenExternalLinks(true);

    auto* composeRow = new QHBoxLayout();
    inputEdit_ = new QTextEdit(rightCard);
    inputEdit_->setPlaceholderText("输入消息...");
    inputEdit_->setFixedHeight(96);

    auto* buttonCol = new QVBoxLayout();
    sendBtn_ = new QPushButton("发送", rightCard);
    sendBtn_->setObjectName("PrimaryBtn");
    sendFileBtn_ = new QPushButton("发送文件", rightCard);
    buttonCol->addWidget(sendBtn_);
    buttonCol->addWidget(sendFileBtn_);
    buttonCol->addStretch(1);

    composeRow->addWidget(inputEdit_, 1);
    composeRow->addLayout(buttonCol);

    rightLayout->addLayout(topRow);
    rightLayout->addWidget(conversationView_, 1);
    rightLayout->addLayout(composeRow);

    root->addWidget(leftCard, 32);
    root->addWidget(rightCard, 68);

    setCentralWidget(central);
    statusBar()->showMessage("准备就绪");

    setStyleSheet(R"(
        QMainWindow { background: #f4f6fa; }
        QFrame#Card {
            background: #ffffff;
            border: 1px solid #e5e7eb;
            border-radius: 12px;
        }
        QLabel#Title {
            font-size: 18px;
            font-weight: 700;
            color: #111827;
        }
        QLabel#Label {
            font-size: 13px;
            font-weight: 600;
            color: #374151;
        }
        QListWidget, QTextBrowser, QLineEdit, QTextEdit {
            border: 1px solid #d1d5db;
            border-radius: 8px;
            background: #ffffff;
            color: #111827;
            font-size: 13px;
        }
        QListWidget::item {
            border-radius: 8px;
            padding: 6px;
        }
        QListWidget::item:selected {
            background: #e0edff;
            color: #0f172a;
        }
        QPushButton {
            min-height: 36px;
            border: 1px solid #d1d5db;
            border-radius: 8px;
            background: #ffffff;
            color: #111827;
            font-weight: 600;
            padding: 0 14px;
        }
        QPushButton:hover { background: #f8fafc; }
        QPushButton#PrimaryBtn {
            background: #2563eb;
            border: 1px solid #1d4ed8;
            color: #ffffff;
        }
        QPushButton#PrimaryBtn:hover { background: #1d4ed8; }
        QStatusBar {
            background: #ffffff;
            border-top: 1px solid #e5e7eb;
            color: #4b5563;
        }
    )");

    const Config config = app_.configCopy();
    nameEdit_->setText(QString::fromStdString(config.userName));
}

void ChatWindow::bindEvents() {
    connect(renameBtn_, &QPushButton::clicked, this, [this]() { onRename(); });
    connect(sendBtn_, &QPushButton::clicked, this, [this]() { onSendMessage(); });
    connect(sendFileBtn_, &QPushButton::clicked, this, [this]() { onSendFile(); });

    connect(contactList_, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem* current, QListWidgetItem*) {
                if (current == nullptr) {
                    activeContactId_.clear();
                    renderCurrentConversation();
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
            });
}

void ChatWindow::onRename() {
    const QString newName = nameEdit_->text().trimmed();
    if (newName.isEmpty()) {
        QMessageBox::warning(this, "提示", "昵称不能为空。");
        return;
    }

    std::string error;
    if (!app_.updateLocalUserName(newName.toStdString(), &error)) {
        QMessageBox::warning(this, "提示", QString::fromStdString(error.empty() ? "昵称保存失败。" : error));
        return;
    }

    updateStatusBar();
    statusBar()->showMessage("昵称已更新", 2000);
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
        return;
    }
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

        if (!newName.isEmpty() && contact.name != newName) {
            contact.name = newName;
            changed = true;
        }
        if (!newIp.isEmpty() && contact.ip != newIp) {
            contact.ip = newIp;
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
        QString text = contact.name.isEmpty() ? contact.userId : contact.name;
        if (!contact.online) {
            text += "（离线）";
        }
        if (contact.unread > 0) {
            text += QString("  [%1]").arg(contact.unread);
        }

        auto* item = new QListWidgetItem(statusIcon(contact.online), text);
        item->setData(Qt::UserRole, contact.userId);
        item->setToolTip(QString("用户ID: %1\nIP: %2").arg(contact.userId, contact.ip));
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

    QString html;
    html += "<html><body style='font-family:Microsoft YaHei UI;font-size:13px;background:#ffffff;'>";
    for (const ChatMessage& message : contact->messages) {
        const QString sender = message.incoming ? (contact->name.isEmpty() ? QString("对方") : contact->name) : QString("我");
        const QString align = message.incoming ? "left" : "right";
        const QString bubbleBg = message.incoming ? "#f3f4f6" : "#dbeafe";
        const QString bubbleColor = "#0f172a";

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
                    "<div style='margin:8px 0;text-align:%1;'>"
                    "<div style='font-size:11px;color:#6b7280;margin-bottom:4px;'>%2  %3</div>"
                    "<div style='display:inline-block;max-width:72%%;padding:8px 10px;border-radius:10px;"
                    "background:%4;color:%5;line-height:1.45;'>%6</div>"
                    "</div>")
                    .arg(align, htmlEscape(sender), timeText(message.timestampMs), bubbleBg, bubbleColor, content);
    }
    html += "</body></html>";

    conversationView_->setHtml(html);
    if (QScrollBar* bar = conversationView_->verticalScrollBar()) {
        bar->setValue(bar->maximum());
    }
}

void ChatWindow::updateStatusBar() {
    const Config config = app_.configCopy();
    const QString text = QString("昵称: %1 | 用户ID: %2 | 数据目录: %3")
                             .arg(QString::fromStdString(config.userName),
                                  QString::fromStdString(config.userId),
                                  QString::fromStdString(app_.dataDirString()));
    statusBar()->showMessage(text);
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

    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
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
    file.write(QJsonDocument(arr).toJson(QJsonDocument::Compact));
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

    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
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
        contact.ip = obj.value("ip").toString();
        contact.online = false;
        contact.unread = obj.value("unread").toInt(0);
        contact.lastSeenMs = static_cast<qint64>(obj.value("last_seen").toDouble());
        loadHistory(contact);
        contacts_.push_back(std::move(contact));
    }

    rebuildContactList();
    renderCurrentConversation();
}

void ChatWindow::saveContacts() const {
    QDir().mkpath(dataDirPath());

    QJsonArray arr;
    for (const Contact& contact : contacts_) {
        QJsonObject obj;
        obj.insert("user_id", contact.userId);
        obj.insert("name", contact.name);
        obj.insert("ip", contact.ip);
        obj.insert("unread", contact.unread);
        obj.insert("last_seen", static_cast<double>(contact.lastSeenMs));
        arr.push_back(obj);
    }

    QFile file(contactFilePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return;
    }
    file.write(QJsonDocument(arr).toJson(QJsonDocument::Indented));
}

QString ChatWindow::dataDirPath() const {
    return QString::fromStdString(app_.dataDirString());
}

QString ChatWindow::chatsDirPath() const {
    return QDir(dataDirPath()).filePath("chats");
}

QString ChatWindow::contactFilePath() const {
    return QDir(dataDirPath()).filePath("contacts.json");
}

QString ChatWindow::historyFilePath(const QString& userId) const {
    const QString token = safeFileToken(userId);
    return QDir(chatsDirPath()).filePath(token + ".json");
}
