#include "lantalk_core.h"

#include <QApplication>
#include <QCloseEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMessageBox>
#include <QPushButton>
#include <QStatusBar>
#include <QTableWidget>
#include <QTextEdit>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <chrono>
#include <stdexcept>

class ChatWindow final : public QMainWindow {
public:
    ChatWindow() {
        setupUi();
        bindEvents();

        app_.setEventCallback([this](const std::string& line) {
            QMetaObject::invokeMethod(
                this,
                [this, line]() {
                    appendLog(QString::fromStdString(line));
                },
                Qt::QueuedConnection);
        });

        if (!app_.init()) {
            throw std::runtime_error(app_.lastError().empty() ? "Initialization failed." : app_.lastError());
        }
        if (!app_.startAsync()) {
            throw std::runtime_error("Failed to start network service.");
        }

        refreshStatusBar();
        refreshPeers();

        peerTimer_ = new QTimer(this);
        peerTimer_->setInterval(1000);
        connect(peerTimer_, &QTimer::timeout, this, [this]() {
            refreshPeers();
            refreshStatusBar();
        });
        peerTimer_->start();
    }

    ~ChatWindow() override {
        app_.setEventCallback(nullptr);
        app_.shutdown();
    }

protected:
    void closeEvent(QCloseEvent* event) override {
        app_.setEventCallback(nullptr);
        app_.shutdown();
        event->accept();
    }

private:
    void setupUi() {
        setWindowTitle("LanTalk");
        resize(1240, 780);

        auto* central = new QWidget(this);
        auto* root = new QHBoxLayout(central);
        root->setContentsMargins(14, 14, 14, 14);
        root->setSpacing(14);

        auto* leftCard = new QFrame(central);
        leftCard->setObjectName("Card");
        auto* leftLayout = new QVBoxLayout(leftCard);
        leftLayout->setContentsMargins(16, 16, 16, 16);
        leftLayout->setSpacing(10);

        auto* title = new QLabel("LanTalk", leftCard);
        title->setObjectName("Title");
        auto* subtitle = new QLabel("Modern LAN Chat for Windows x64", leftCard);
        subtitle->setObjectName("Subtitle");
        auto* peerLabel = new QLabel("Online Users", leftCard);
        peerLabel->setObjectName("Section");

        peerTable_ = new QTableWidget(leftCard);
        peerTable_->setColumnCount(4);
        peerTable_->setHorizontalHeaderLabels({"Name", "User ID", "Address", "Seen"});
        peerTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
        peerTable_->setSelectionMode(QAbstractItemView::SingleSelection);
        peerTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        peerTable_->setAlternatingRowColors(true);
        peerTable_->verticalHeader()->setVisible(false);
        peerTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        peerTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
        peerTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
        peerTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);

        leftLayout->addWidget(title);
        leftLayout->addWidget(subtitle);
        leftLayout->addSpacing(6);
        leftLayout->addWidget(peerLabel);
        leftLayout->addWidget(peerTable_, 1);

        auto* rightCard = new QFrame(central);
        rightCard->setObjectName("Card");
        auto* rightLayout = new QVBoxLayout(rightCard);
        rightLayout->setContentsMargins(16, 16, 16, 16);
        rightLayout->setSpacing(12);

        auto* topRow = new QHBoxLayout();
        topRow->setSpacing(10);
        auto* nameLabel = new QLabel("Display Name", rightCard);
        nameLabel->setObjectName("Section");
        nameEdit_ = new QLineEdit(rightCard);
        nameEdit_->setPlaceholderText("Your nickname");
        renameBtn_ = new QPushButton("Set Name", rightCard);
        renameBtn_->setObjectName("PrimaryBtn");

        topRow->addWidget(nameLabel);
        topRow->addStretch(1);
        topRow->addWidget(nameEdit_, 1);
        topRow->addWidget(renameBtn_);

        auto* chatLabel = new QLabel("Conversation", rightCard);
        chatLabel->setObjectName("Section");

        logView_ = new QTextEdit(rightCard);
        logView_->setReadOnly(true);
        logView_->setAcceptRichText(false);

        auto* composerRow = new QHBoxLayout();
        composerRow->setSpacing(10);
        inputEdit_ = new QLineEdit(rightCard);
        inputEdit_->setPlaceholderText("Type message...");
        sendBtn_ = new QPushButton("Send", rightCard);
        sendBtn_->setObjectName("PrimaryBtn");
        fileBtn_ = new QPushButton("Send File", rightCard);

        composerRow->addWidget(inputEdit_, 1);
        composerRow->addWidget(sendBtn_);
        composerRow->addWidget(fileBtn_);

        rightLayout->addLayout(topRow);
        rightLayout->addWidget(chatLabel);
        rightLayout->addWidget(logView_, 1);
        rightLayout->addLayout(composerRow);

        root->addWidget(leftCard, 36);
        root->addWidget(rightCard, 64);

        setCentralWidget(central);
        statusBar()->showMessage("Ready");

        setStyleSheet(R"(
            QMainWindow {
                background: #f4f6f9;
            }
            QFrame#Card {
                background: #ffffff;
                border: 1px solid #e5e7eb;
                border-radius: 12px;
            }
            QLabel#Title {
                font-size: 24px;
                font-weight: 700;
                color: #111827;
            }
            QLabel#Subtitle {
                font-size: 13px;
                color: #6b7280;
            }
            QLabel#Section {
                font-size: 13px;
                font-weight: 600;
                color: #1f2937;
            }
            QTableWidget, QTextEdit, QLineEdit {
                border: 1px solid #d1d5db;
                border-radius: 8px;
                background: #ffffff;
                color: #111827;
                font-size: 13px;
                padding: 6px;
            }
            QTableWidget {
                gridline-color: #eef1f5;
            }
            QHeaderView::section {
                background: #f9fafb;
                border: none;
                border-bottom: 1px solid #e5e7eb;
                padding: 6px;
                color: #374151;
                font-weight: 600;
            }
            QPushButton {
                min-height: 34px;
                border: 1px solid #d1d5db;
                border-radius: 8px;
                background: #ffffff;
                color: #111827;
                font-weight: 600;
                padding: 0 14px;
            }
            QPushButton:hover {
                background: #f9fafb;
            }
            QPushButton#PrimaryBtn {
                background: #2563eb;
                border: 1px solid #1d4ed8;
                color: #ffffff;
            }
            QPushButton#PrimaryBtn:hover {
                background: #1d4ed8;
            }
            QStatusBar {
                background: #ffffff;
                border-top: 1px solid #e5e7eb;
                color: #4b5563;
            }
        )");

        const Config cfg = app_.configCopy();
        nameEdit_->setText(QString::fromStdString(cfg.userName));
    }

    void bindEvents() {
        connect(sendBtn_, &QPushButton::clicked, this, [this]() { onSendMessage(); });
        connect(fileBtn_, &QPushButton::clicked, this, [this]() { onSendFile(); });
        connect(renameBtn_, &QPushButton::clicked, this, [this]() { onRename(); });
        connect(inputEdit_, &QLineEdit::returnPressed, this, [this]() { onSendMessage(); });
        connect(nameEdit_, &QLineEdit::returnPressed, this, [this]() { onRename(); });
    }

    void appendLog(const QString& line) {
        logView_->append(line);
    }

    void refreshStatusBar() {
        const Config cfg = app_.configCopy();
        const QString text = QString("Local: %1 | UserID: %2 | Data: %3")
                                 .arg(QString::fromStdString(cfg.userName),
                                      QString::fromStdString(cfg.userId),
                                      QString::fromStdString(app_.dataDirString()));
        statusBar()->showMessage(text);
    }

    void refreshPeers() {
        QString selectedUserId;
        const int row = peerTable_->currentRow();
        if (row >= 0 && row < peerTable_->rowCount()) {
            selectedUserId = peerTable_->item(row, 1)->text();
        }

        const auto peers = app_.snapshotPeers();
        const auto now = std::chrono::steady_clock::now();

        peerTable_->setRowCount(0);
        for (const auto& peer : peers) {
            const int newRow = peerTable_->rowCount();
            peerTable_->insertRow(newRow);

            auto* nameItem = new QTableWidgetItem(QString::fromStdString(peer.name));
            auto* userIdItem = new QTableWidgetItem(QString::fromStdString(peer.userId));
            auto* addrItem = new QTableWidgetItem(QString::fromStdString(peer.ip + ":" + std::to_string(peer.port)));
            const auto age = std::chrono::duration_cast<std::chrono::seconds>(now - peer.lastSeen).count();
            auto* seenItem = new QTableWidgetItem(QString::number(age) + "s");

            peerTable_->setItem(newRow, 0, nameItem);
            peerTable_->setItem(newRow, 1, userIdItem);
            peerTable_->setItem(newRow, 2, addrItem);
            peerTable_->setItem(newRow, 3, seenItem);

            if (!selectedUserId.isEmpty() && userIdItem->text() == selectedUserId) {
                peerTable_->selectRow(newRow);
            }
        }

        if (peerTable_->currentRow() < 0 && peerTable_->rowCount() > 0) {
            peerTable_->selectRow(0);
        }
    }

    bool selectedPeerUserId(std::string& outUserId) const {
        const int row = peerTable_->currentRow();
        if (row < 0 || row >= peerTable_->rowCount()) {
            return false;
        }
        auto* item = peerTable_->item(row, 1);
        if (item == nullptr || item->text().isEmpty()) {
            return false;
        }
        outUserId = item->text().toStdString();
        return true;
    }

    QString peerNameByUserId(const std::string& userId) {
        const auto peers = app_.snapshotPeers();
        for (const auto& peer : peers) {
            if (peer.userId == userId) {
                return QString::fromStdString(peer.name);
            }
        }
        return QString::fromStdString(userId);
    }

    void onSendMessage() {
        std::string userId;
        if (!selectedPeerUserId(userId)) {
            QMessageBox::information(this, "LanTalk", "Please select a peer first.");
            return;
        }

        const QString text = inputEdit_->text().trimmed();
        if (text.isEmpty()) {
            return;
        }

        std::string errorText;
        if (!app_.sendTextToUserId(userId, text.toStdString(), &errorText)) {
            QMessageBox::critical(this, "LanTalk", QString::fromStdString(errorText));
            refreshPeers();
            return;
        }

        appendLog(QString("[Sent] to %1: %2").arg(peerNameByUserId(userId), text));
        inputEdit_->clear();
    }

    void onSendFile() {
        std::string userId;
        if (!selectedPeerUserId(userId)) {
            QMessageBox::information(this, "LanTalk", "Please select a peer first.");
            return;
        }

        const QString filePath = QFileDialog::getOpenFileName(this, "Select File", QString(), "All Files (*.*)");
        if (filePath.isEmpty()) {
            return;
        }

        std::string errorText;
        if (!app_.sendFileToUserId(userId, fs::path(filePath.toStdString()), &errorText)) {
            QMessageBox::critical(this, "LanTalk", QString::fromStdString(errorText));
            refreshPeers();
            return;
        }

        appendLog(QString("[Sent file] to %1: %2").arg(peerNameByUserId(userId), QFileInfo(filePath).fileName()));
    }

    void onRename() {
        const QString name = nameEdit_->text().trimmed();
        std::string errorText;
        if (!app_.updateLocalUserName(name.toStdString(), &errorText)) {
            QMessageBox::critical(this, "LanTalk", QString::fromStdString(errorText));
            return;
        }
        refreshStatusBar();
    }

private:
    LanTalkApp app_;

    QTableWidget* peerTable_ = nullptr;
    QTextEdit* logView_ = nullptr;
    QLineEdit* inputEdit_ = nullptr;
    QLineEdit* nameEdit_ = nullptr;
    QPushButton* sendBtn_ = nullptr;
    QPushButton* fileBtn_ = nullptr;
    QPushButton* renameBtn_ = nullptr;

    QTimer* peerTimer_ = nullptr;
};

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("LanTalk");

    try {
        ChatWindow window;
        window.show();
        return app.exec();
    } catch (const std::exception& ex) {
        QMessageBox::critical(nullptr, "LanTalk", QString::fromUtf8(ex.what()));
        return 1;
    }
}
