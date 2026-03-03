#include "chat_window.h"

#include <QApplication>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QMessageBox>

#include <exception>

int main(int argc, char* argv[]) {
    // Keep fractional DPI scaling (e.g. 125%/150%) instead of rounding down.
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
    QApplication app(argc, argv);
    app.setApplicationName("LanTalk");
    const int fontId = QFontDatabase::addApplicationFont(":/fonts/LXGWWenKaiScreen.ttf");
    if (fontId >= 0) {
        const QStringList families = QFontDatabase::applicationFontFamilies(fontId);
        if (!families.isEmpty()) {
            QFont font(families.constFirst());
            font.setPointSize(11);
            font.setStyleStrategy(QFont::PreferQuality);
            font.setHintingPreference(QFont::PreferFullHinting);
            app.setFont(font);
        }
    }
    app.setStyleSheet(R"(
        QMessageBox QLabel {
            font-size: 14px;
            min-width: 280px;
        }
        QMessageBox QPushButton {
            min-width: 84px;
            min-height: 32px;
            font-size: 13px;
            padding: 0 12px;
        }
    )");

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
