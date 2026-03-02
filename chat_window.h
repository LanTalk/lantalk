#pragma once

#include "lantalk_core.h"

#include <QMainWindow>
#include <QByteArray>
#include <QString>

#include <cstdint>
#include <vector>

class QListWidget;
class QListWidgetItem;
class QCloseEvent;
class QPushButton;
class QToolButton;
class QTextBrowser;
class QTextEdit;
class QTimer;

class ChatWindow final : public QMainWindow {
public:
    ChatWindow();
    ~ChatWindow() override;

protected:
    void closeEvent(QCloseEvent* event) override;

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
        QString ip;
        bool online = false;
        int unread = 0;
        qint64 lastSeenMs = 0;
        std::vector<ChatMessage> messages;
    };

    void setupUi();
    void bindEvents();

    void onSendMessage();
    void onSendFile();
    void openSettingsDialog();

    void refreshOnlinePeers();
    void rebuildContactList();
    void renderCurrentConversation();

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
    void applySelfAvatar();
    QString localIpSummary() const;

    uint64_t storageSeed() const;
    QByteArray encryptBlob(const QByteArray& plain) const;
    QByteArray decryptBlob(const QByteArray& blob) const;

    LanTalkApp app_;
    QTimer* refreshTimer_ = nullptr;

    QPushButton* selfAvatarBtn_ = nullptr;
    QToolButton* settingsBtn_ = nullptr;
    QListWidget* contactList_ = nullptr;
    QTextBrowser* conversationView_ = nullptr;
    QTextEdit* inputEdit_ = nullptr;
    QPushButton* sendBtn_ = nullptr;
    QPushButton* sendFileBtn_ = nullptr;

    std::vector<Contact> contacts_;
    QString activeContactId_;
    QString selfAvatarPath_;
};
