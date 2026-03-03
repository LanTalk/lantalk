#include "chat_window.h"

#include <QApplication>
#include <QFontDatabase>
#include <QMessageBox>

#include <exception>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("LanTalk");
    const int fontId = QFontDatabase::addApplicationFont(":/fonts/LXGWWenKaiScreen.ttf");
    if (fontId >= 0) {
        const QStringList families = QFontDatabase::applicationFontFamilies(fontId);
        if (!families.isEmpty()) {
            QFont font(families.constFirst());
            font.setStyleStrategy(QFont::PreferAntialias);
            app.setFont(font);
        }
    }

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
