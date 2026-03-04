#pragma once

#include "lantalk_core.h"

#include <QEvent>
#include <QMainWindow>
#include <QByteArray>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <vector>

class QListWidget;
class QListWidgetItem;
class QCloseEvent;
class QLabel;
class QLineEdit;
class QPushButton;
class QResizeEvent;
class QShowEvent;
class QToolButton;
class QTextEdit;
class QTimer;
class QWidget;
class QScrollArea;
class QVBoxLayout;
class QNetworkAccessManager;
class QNetworkReply;

class ChatWindow final : public QMainWindow {
public:
    ChatWindow();
    ~ChatWindow() override;

protected:
    void closeEvent(QCloseEvent* event) override;
    void changeEvent(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    enum class PresenceKind {
        Offline = 0,
        Lan = 1,
        SignalP2P = 2,
        SignalWs = 3,
    };

    struct ChatMessage {
        qint64 timestampMs = 0;
        bool incoming = false;
        bool isFile = false;
        QString text;
        QString fileName;
        QString filePath;
    };

    struct Contact {
        QString userId;
        QString name;
        QString remark;
        QString ip;
        QString avatarPayload;
        bool online = false;
        bool lanOnline = false;
        bool signalOnline = false;
        PresenceKind presence = PresenceKind::Offline;
        QString signalMode;
        QString signalServer;
        int unread = 0;
        qint64 lastSeenMs = 0;
        std::vector<ChatMessage> messages;
    };

    void setupUi();
    void bindEvents();

    void onSendMessage();
    void onSendFile();
    void openEmojiMenu();
    void openSettingsDialog();
    void openContactProfileDialog();

    void refreshOnlinePeers();
    void refreshSignalingPeers();
    void pollSignalMessages();
    void syncSignalPresence(bool* changed);
    void noteVerifiedSignalP2P(const QString& userId);
    bool hasRecentVerifiedSignalP2P(const QString& userId, qint64 nowMsValue) const;
    QJsonArray buildVerifiedSignalP2PPeers(qint64 nowMsValue) const;
    void appendSignalOutgoingMessage(Contact& contact, const QString& text, qint64 timestampMs);
    void rebuildContactList();
    void renderCurrentConversation();
    void updateChatHeader();

    void handleMessageEvent(const MessageEvent& event);

    Contact* findContact(const QString& userId);
    const Contact* findContact(const QString& userId) const;
    Contact& ensureContact(const QString& userId);
    QString currentContactId() const;

    void loadContacts();
    void saveContacts() const;
    void loadHistory(Contact& contact);
    void saveHistory(const Contact& contact) const;

    QString dataDirPath() const;
    QString chatsDirPath() const;
    QString contactFilePath() const;
    QString historyFilePath(const QString& userId) const;
    QString profileFilePath() const;

    void loadProfile();
    void saveProfile() const;
    void ensureDefaultAvatarLibrary();
    QByteArray buildAvatarPayload(const QString& avatarPath) const;
    void syncLocalAvatarToNetwork();
    QStringList normalizeSignalServers(const QStringList& rawServers) const;
    bool signalRequest(const QString& url,
                       const QString& method,
                       const QJsonObject* body,
                       QJsonDocument* outDoc,
                       QString* errorText) const;
    QString signalApiUrl(const QString& serverBase, const QString& path) const;
    void applySelfAvatar();
    QString localIpSummary() const;
    QString displayName(const Contact& contact) const;
    QString presenceText(PresenceKind kind) const;
    void refreshWindowBorder();

    uint64_t storageSeed() const;
    QByteArray encryptBlob(const QByteArray& plain) const;
    QByteArray decryptBlob(const QByteArray& blob) const;

    LanTalkApp app_;
    QTimer* refreshTimer_ = nullptr;

    QWidget* titleBar_ = nullptr;
    QWidget* windowBorder_ = nullptr;
    QWidget* railPane_ = nullptr;
    QWidget* contactsTopDrag_ = nullptr;
    QLabel* chatTitleLabel_ = nullptr;
    QPushButton* minBtn_ = nullptr;
    QPushButton* maxBtn_ = nullptr;
    QPushButton* closeBtn_ = nullptr;
    QPushButton* viewProfileBtn_ = nullptr;

    QPushButton* selfAvatarBtn_ = nullptr;
    QToolButton* settingsBtn_ = nullptr;
    QToolButton* emojiBtn_ = nullptr;
    QLineEdit* searchEdit_ = nullptr;
    QListWidget* contactList_ = nullptr;
    QScrollArea* conversationView_ = nullptr;
    QWidget* conversationList_ = nullptr;
    QVBoxLayout* conversationLayout_ = nullptr;
    QTextEdit* inputEdit_ = nullptr;
    QPushButton* sendBtn_ = nullptr;
    QPushButton* sendFileBtn_ = nullptr;

    std::vector<Contact> contacts_;
    QString activeContactId_;
    QString selfAvatarPath_;
    QStringList defaultAvatarPaths_;
    QStringList signalingServers_;
    QNetworkAccessManager* signalingNet_ = nullptr;
    QTimer* signalingTimer_ = nullptr;
    QHash<QString, qint64> signalAfterByServer_;
    QHash<QString, QString> signalServerByUserId_;
    QHash<QString, qint64> verifiedSignalP2PAtMsByUserId_;
    QSet<QString> signalKnownUsers_;
    QSet<QString> signalP2PUsers_;
    QSet<QString> signalWsUsers_;
    QSet<QString> seenSignalMsgIds_;
};
