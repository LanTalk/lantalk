#include "chat_window.h"

#include <QApplication>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QMessageBox>

#include <chrono>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

#include <windows.h>

namespace fs = std::filesystem;

namespace {
std::string nowLocalTimestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_s(&tm, &t);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

void appendStartupLog(const std::string& line) {
    std::error_code ec;
    fs::path dataDir = fs::current_path(ec) / "data";
    if (ec) {
        return;
    }
    fs::create_directories(dataDir, ec);
    if (ec) {
        return;
    }
    std::ofstream out(dataDir / "startup.log", std::ios::out | std::ios::app);
    if (!out) {
        return;
    }
    out << "[" << nowLocalTimestamp() << "] " << line << '\n';
}
}  // namespace

int main(int argc, char* argv[]) {
    appendStartupLog("process start pid=" + std::to_string(GetCurrentProcessId()));
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::RoundPreferFloor);
    QApplication app(argc, argv);
    app.setApplicationName("LanTalk");
    appendStartupLog("qapplication initialized");
    const int fontId = QFontDatabase::addApplicationFont(":/fonts/LXGWWenKaiScreen.ttf");
    if (fontId >= 0) {
        const QStringList families = QFontDatabase::applicationFontFamilies(fontId);
        if (!families.isEmpty()) {
            QFont font(families.constFirst());
            font.setPointSize(10);
            font.setStyleStrategy(QFont::PreferQuality);
            font.setHintingPreference(QFont::PreferFullHinting);
            app.setFont(font);
        }
    }

    try {
        NetworkRuntime runtime;
        (void)runtime;
        appendStartupLog("winsock runtime initialized");

        ChatWindow window;
        appendStartupLog("chat window initialized");
        window.show();
        appendStartupLog("window shown, entering event loop");
        return app.exec();
    } catch (const std::exception& ex) {
        appendStartupLog(std::string("fatal std::exception: ") + ex.what());
        QMessageBox::critical(nullptr, "LanTalk", QString::fromUtf8(ex.what()));
        return 1;
    } catch (...) {
        appendStartupLog("fatal unknown exception");
        QMessageBox::critical(nullptr, "LanTalk", QString::fromUtf8("启动失败：未知错误。请查看 data/startup.log。"));
        return 1;
    }
}
