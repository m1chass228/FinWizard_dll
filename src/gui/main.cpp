#include <QApplication>
#include <QDebug>
#include <QWidget>
#include <QStyleFactory>
#include <QStyle>
#include <finwizard/pluginmanager.h>

#include "mainwindow.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    // Fusion отлично подходит для кастомизации и кроссплатформенности
    QApplication::setStyle(QStyleFactory::create("Fusion"));

    QCoreApplication::setOrganizationName("FinWizard");
    QCoreApplication::setApplicationName("FinWizardGui");

    qDebug() << "FinWizard dll запущен!";

    // ФИКС УТЕЧКИ: Создаём менеджер на стеке (или используй умный указатель),
    // чтобы он гарантированно удалился при выходе из app.exec()
    PluginManager manager;

    // Внедряем универсальный стиль для ховера при Drag-and-Drop
    QString globalStyle = R"(
        /* Подсвечиваем только границы, оставляя фон системным */
        QComboBox[dragHover="true"] {
            border: 2px solid #2ecc71;
            border-radius: 4px;
        }
        QListWidget[dragHover="true"] {
            border: 2px solid #3498db;
            border-radius: 4px;
        }

        /* Шрифт для логов */
        QTextEdit#logTextEdit {
            font-family: "Consolas", "Monospace", "Courier New";
        }
    )";
    app.setStyleSheet(globalStyle);

    // Передаем адрес объекта, созданного на стеке
    MainWindow window(&manager);
    window.show();

    return app.exec();
}