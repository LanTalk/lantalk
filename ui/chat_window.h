#pragma once

#include "../lantalk_core.h"

#include <QMainWindow>

#include <cstdint>
#include <vector>

class QListWidget;
class QListWidgetItem;
class QCloseEvent;
class QLineEdit;
class QPushButton;
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

    void onRename();
    void onSendMessage();
    void onSendFile();

    void refreshOnlinePeers();
    void rebuildContactList();
    void renderCurrentConversation();
    void updateStatusBar();

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

    LanTalkApp app_;
    QTimer* refreshTimer_ = nullptr;

    QListWidget* contactList_ = nullptr;
    QLineEdit* nameEdit_ = nullptr;
    QPushButton* renameBtn_ = nullptr;
    QTextBrowser* conversationView_ = nullptr;
    QTextEdit* inputEdit_ = nullptr;
    QPushButton* sendBtn_ = nullptr;
    QPushButton* sendFileBtn_ = nullptr;

    std::vector<Contact> contacts_;
    QString activeContactId_;
};
