#pragma once

#include "lantalk_core.h"

#include <QEvent>
#include <QMainWindow>
#include <QByteArray>
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
class QToolButton;
class QTextBrowser;
class QTextEdit;
class QTimer;
class QWidget;

class ChatWindow final : public QMainWindow {
public:
    ChatWindow();
    ~ChatWindow() override;

protected:
    void closeEvent(QCloseEvent* event) override;
    void changeEvent(QEvent* event) override;
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
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
    void applySelfAvatar();
    QString localIpSummary() const;
    QString displayName(const Contact& contact) const;

    uint64_t storageSeed() const;
    QByteArray encryptBlob(const QByteArray& plain) const;
    QByteArray decryptBlob(const QByteArray& blob) const;

    LanTalkApp app_;
    QTimer* refreshTimer_ = nullptr;

    QWidget* titleBar_ = nullptr;
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
    QTextBrowser* conversationView_ = nullptr;
    QTextEdit* inputEdit_ = nullptr;
    QPushButton* sendBtn_ = nullptr;
    QPushButton* sendFileBtn_ = nullptr;

    std::vector<Contact> contacts_;
    QString activeContactId_;
    QString selfAvatarPath_;
    QStringList defaultAvatarPaths_;
};
