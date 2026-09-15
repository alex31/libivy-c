#include "window.hpp"
#include <QApplication>
#include <QDebug>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    MainWindow window;
    const auto address = argc > 1 ? std::optional<std::string_view>(argv[1]) : std::nullopt;
    if (auto result = window.start(address); !result) {
        qCritical().noquote() << "Impossible de démarrer Ivy:" << QString::fromStdString(result.error().message());
        return 1;
    }
    window.show();
    return app.exec();
}
