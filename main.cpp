#include "chat_window.h"

#include <QApplication>
#include <QMessageBox>

#include <exception>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("LanTalk");

    try {
        NetworkRuntime runtime;
        (void)runtime;

        ChatWindow window;
        window.show();
        return app.exec();
    } catch (const std::exception& ex) {
        QMessageBox::critical(nullptr, "LanTalk", QString::fromUtf8(ex.what()));
        return 1;
    }
}
