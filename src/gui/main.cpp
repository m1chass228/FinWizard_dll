#include <QApplication>
#include <QDebug>
#include <QWidget>
#include <QStyleFactory>
#include <QStyle>
#include <QPushButton>
#include <QComboBox>
#include <QListView>
#include <QTimer>

// Заголовочные файлы для анимаций и эффектов
#include <QPropertyAnimation>
#include <QGraphicsOpacityEffect>
#include <QGraphicsDropShadowEffect>

#include <finwizard/pluginmanager.h>
#include "mainwindow.h"

// ==========================================
//               MAIN FUNCTION
// ==========================================

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    QApplication::setStyle(QStyleFactory::create("Fusion"));

    QCoreApplication::setOrganizationName("FinWizard");
    QCoreApplication::setApplicationName("FinWizardGui");

    qDebug() << "FinWizard dll запущен!";

    PluginManager manager;

    // === ОБРАБОТКА АРГУМЕНТОВ КОМАНДНОЙ СТРОКИ (.fwp файлы) ===
    QString droppedFilePath;
    if (argc > 1) {
        QString arg1 = QString::fromLocal8Bit(argv[1]);
        if (arg1.endsWith(".fwp", Qt::CaseInsensitive)) {
            droppedFilePath = arg1;
            qDebug() << "Открыт .fwp файл через ассоциацию:" << droppedFilePath;
        }
    }

    // Обновлённый стиль с поддержкой Micro-UX и стилизацией ComboBox
    QString globalStyle = R"(

/* === ОСНОВА === */
QWidget {
    font-size: 10pt;
}

/* === КАРТОЧКИ === */
QFrame#pluginDropZone,
QFrame#xlsxDropZone {
    background-color: palette(base);
    border: 1px solid palette(mid);
    border-radius: 12px;
    padding: 14px;
}

/* Hover (drag) */
QFrame[dragHover="true"] {
    border: 2px solid #2ecc71;
    background-color: rgba(46, 204, 113, 0.05);
}

/* === ПРЕВЬЮ КОНФИГА === */

/* Карточка иконка+название+описание — та же рамка, что у QListWidget, чтобы весь
   блок превью читался единым целым и отделялся от остального пространства панели */
QFrame#configPreviewCard {
    border: 1px solid palette(mid);
    border-radius: 8px;
    background-color: palette(base);
}

QLabel#configIcon {
    min-width: 80px;
    min-height: 80px;
    border: 1px dashed palette(mid);
    border-radius: 10px;
    background-color: palette(window);
    padding: 6px; /* чтобы картинка не перекрывала скругление/рамку своими острыми углами */
}

QLabel#configName {
    font-size: 17px;
    font-weight: 600;
    color: palette(text);
}

QLabel#configDescription {
    /* ИСПРАВЛЕНИЕ: palette(mid) — это роль для бордеров/теней, специально низкоконтрастная,
       для текста она сливается с фоном что на темной, что на светлой теме. palette(text) —
       гарантированный контраст на любой теме; "второстепенность" даем через размер/стиль. */
    color: palette(text);
    font-size: 9pt;
    font-style: italic;
    line-height: 1.3;
}

/* Пунктирные разделители в карточке превью: между иконкой и именем (вертикальный),
   и между блоком иконка+имя и описанием (горизонтальный) — для визуального баланса */
QFrame#configPreviewDivider {
    border: none;
    border-top: 1px dashed palette(mid);
    margin: 2px 0px;
}

QFrame#configPreviewVDivider {
    border: none;
    border-left: 1px dashed palette(mid);
    margin: 0px 2px;
}

/* === КНОПКИ === */
QPushButton {
    border-radius: 8px;
    padding: 8px 16px;
    border: 1px solid palette(mid);
    background-color: palette(button);
}

QPushButton:hover {
    background-color: palette(light);
    border: 1px solid #2ecc71;
}

QPushButton#startButton {
    background-color: #2ecc71;
    color: white;
    border: none;
    font-weight: bold;
    padding: 12px 40px;
}

QPushButton#startButton:hover {
    background-color: #3eea83;
}

QPushButton#startButton:disabled {
    background-color: #95a5a6;
}

/* === INPUT === */
QLineEdit {
    padding: 10px;
    border-radius: 8px;
    border: 1px solid palette(mid);
    background-color: palette(base);
}

QLineEdit:focus {
    border: 1px solid #2ecc71;
}

/* === LIST === */
QListWidget {
    border: 1px solid palette(mid);
    border-radius: 8px;
    padding: 6px;
}

/* === ЛОГИ === */
QTextEdit#logTextEdit {
    background-color: palette(base);
    color: palette(text);
    border-radius: 8px;
    border: 1px solid palette(mid);
}

/* === РАЗДЕЛИТЕЛЬ === */
QFrame#frame {
    background: palette(mid);
    margin: 8px 0;
    max-height: 1px;
}

/* === MODERN COMBOBOX === */
QComboBox {
    border: 1px solid palette(mid);
    border-radius: 8px;
    padding: 8px 12px;
    background-color: palette(base);
    min-height: 32px;
    /* ИСПРАВЛЕНИЕ ПРИЗРАЧНОГО БЕЛОГО БЛОКА В ПОПАПЕ: как только на QComboBox вешается
       QSS, Qt по умолчанию рисует попап через "styled" режим, который резервирует под
       текущий выбранный пункт рамку в духе editable-поля — она и торчит пустым белым
       прямоугольником прямо под текущим значением. combobox-popup: 0 — официальное
       Qt Style Sheets свойство, которое возвращает попап к простому "плоскому" списку
       без этой рамки. Это не хак, это задокументированное поведение Qt именно под
       этот баг. */
    combobox-popup: 0;
}

QComboBox:hover {
    border: 1px solid #2ecc71;
    background-color: palette(light);
}

QComboBox::drop-down {
    /* ИСПРАВЛЕНИЕ "ОСТРЫХ УГЛОВ": без явного border-radius тут внутренняя область
       стрелочки-кнопки красится прямоугольным сабконтролом поверх скругленного
       общего контура QComboBox — по углам торчат квадратные "уголки". Скругляем
       ровно на тот же радиус, что и весь combobox, чтобы силуэт совпадал целиком. */
    border: none;
    width: 28px;
    background: transparent;
    border-top-right-radius: 8px;
    border-bottom-right-radius: 8px;
}

QComboBox QAbstractItemView {
    /* ИСПРАВЛЕНИЕ "ЧЕРНЫХ УГЛОВ": попап комбобокса — отдельное top-level окно
       (Qt::Popup), и border-radius скругляет только контент внутри, а не форму
       самого окна у оконного менеджера — она остается прямоугольной. По углам
       "просвечивает" квадрат под скруглением. Скругление popup-окон в Qt Widgets
       требует WA_TranslucentBackground + маску и ненадежно работает на разных
       ОС/композиторах — проще и стабильнее просто не скруглять popup вообще.
       Скругления внутри самого приложения (карточки, кнопки, комбобокс закрытый)
       на форму окна не влияют и остаются как есть. */
    border: 1px solid palette(mid);
    background-color: palette(base);
    selection-background-color: #2ecc71;
    selection-color: white;
    padding: 6px;
    outline: none;
}

)";
    app.setStyleSheet(globalStyle);

    MainWindow window(&manager);
    window.show();

    // === АВТОМАТИЧЕСКОЕ ДОБАВЛЕНИЕ .fwp ПРИ ЗАПУСКЕ ЧЕРЕЗ АССОЦИАЦИЮ ===
    if (!droppedFilePath.isEmpty()) {
        QTimer::singleShot(500, &window, [&window, &manager, droppedFilePath]() {
            QPair<int, QString> res = manager.addConfigFromArchive(droppedFilePath);
            if (res.first != -1) {
                qDebug() << "Плагин успешно добавлен из .fwp файла. ID:" << res.first;
            } else {
                qDebug() << "Ошибка добавления .fwp:" << res.second;
            }
        });
    }

    return app.exec();
}