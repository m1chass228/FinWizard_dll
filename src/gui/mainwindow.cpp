// src/gui/mainwindow.cpp
#include "mainwindow.h"
#include "finwizard/pluginmanager.h"
#include "finwizard/archivemanager.h"
#include "ui_mainwindow.h"

#include <QMessageBox>
#include <QFileDialog>
#include <QDebug>
#include <QDesktopServices>
#include <QStandardPaths>
#include <QSettings>
#include <QFileInfo>
#include <QDir>
#include <QTemporaryDir>
#include <QDirIterator>
#include <QFileSystemWatcher>
#include <QCheckBox>
#include <QLineEdit>
#include <QTimer>
#include <QMenu>
#include <QClipboard>
#include <QMimeData>
#include <QStandardItemModel>
#include <QStandardItem>
#include <QDropEvent>
#include <QPainter>
#include <QMimeData>
#include <QUrl>

MainWindow::MainWindow(PluginManager *pluginManager, QWidget *parent)
    : QMainWindow(parent)
    , m_pluginManager(pluginManager)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    setAcceptDrops(true);

    setWindowTitle("FinWizard dll v1.2.0");
    setWindowIcon(QIcon(":/res/icon.png"));

    if (!m_pluginManager) {
        qCritical() << "PluginManager is null!";
        return;
    }

    // Кнопка выключена, пока не выбран конфиг
    //ui->startButton->setEnabled(false);

    // Включаем поддержку HTML тегов, так как acceptRichText в .ui выключен
    ui->logTextEdit->setAcceptRichText(true);

    logMessage("Программа запущена. Конфиги загружены из кэша.", false);

    // --- 7. ЗАПРЕТ НА ЗАПРЕЩЕННЫЕ СИМВОЛЫ ---
    QRegularExpression fileRegex("[^\\*\\?\\\"<>\\|/:]*");
    QRegularExpressionValidator *validator = new QRegularExpressionValidator(fileRegex, this);
    ui->fileName->setValidator(validator);

    // Пошаговый запуск
    initSettingsAndPaths();       // 1. Считали пути в m_configsPath и m_inputPath
    setupCoreComponents();        // 2. Создали таймер и вотчер
    setupWidgets();               // 3. Стили и mouse tracking для ComboBox
    setupConnections();           // 4. Связали всё сигналами

    // Первая отрисовка
    updateConfigList();
    updateXlsxList();
    updateInterfaceIcons();
}

MainWindow::~MainWindow()
{
    delete ui;
}

void MainWindow::initSettingsAndPaths()
{
    // --- 1. ПУТИ И НАСТРОЙКИ ---
    QSettings settings("FinWizard", "Settings");

    QString defaultCachePath;
    QString defaultInputPath;

#if defined(Q_OS_WIN)
    // Для Windows уходим от C:/Users/Кириллица/AppData во избежание проблем с venv и кодировками
    defaultCachePath = "C:/ProgramData/FinWizard/plugins-cache";
    defaultInputPath = "C:/ProgramData/FinWizard/Input";
#else
    // Для Linux и остальных ОС оставляем стандартные пути в домашней папке
    defaultCachePath = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/FinWizard/plugins-cache";
    defaultInputPath = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/FinWizard_Input";
#endif

    m_configsPath = settings.value("cache/path", defaultCachePath).toString();
    m_inputPath = settings.value("inputFolder", defaultInputPath).toString();

    QDir().mkpath(m_configsPath);
    QDir().mkpath(m_inputPath);

    // --- 2. ИНИЦИАЛИЗАЦИЯ МЕНЕДЖЕРА ---
    m_pluginManager->setCacheBasePath(m_configsPath);
    m_pluginManager->refreshPlugins();
}

void MainWindow::setupCoreComponents()
{
    m_watchdogTimer = new QTimer(this);
    m_watchdogTimer->setSingleShot(true);
    m_watchdogTimer->setInterval(500);

    m_watcher = new QFileSystemWatcher(this);
    m_watcher->addPath(m_configsPath);
    m_watcher->addPath(m_inputPath);
}

void MainWindow::setupWidgets()
{
    QListView* popupListView = qobject_cast<QListView*>(ui->configComboBox->view());
    if (popupListView) {
        popupListView->setMouseTracking(true); // Включаем отслеживание мыши для hover-эффектов

        // Включаем поддержку кастомных стилей для отображения элементов
        popupListView->setStyleSheet(
            "QListView::item {"
            "    padding: 8px;"
            "    border-bottom: 1px solid rgba(255,255,255,0.05);"
            "}"
            "QListView::item:hover {"
            "    background-color: #2c3e50;"
            "}"
            );
    }

    // --- 4. КОНТЕКСТНОЕ МЕНЮ ---
    ui->xlsxList->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(ui->xlsxList, &QListWidget::customContextMenuRequested, this, &MainWindow::showXlsxContextMenu);

    // --- НАСТРОЙКА УДАЛЕНИЯ ПЛАГИНОВ ПО ПРАВОМУ КЛИКУ В СПИСКЕ ---
    if (ui->configComboBox && ui->configComboBox->view()) {
        QAbstractItemView* comboBoxView = ui->configComboBox->view();

        // Разрешаем кастомное контекстное меню для всплывающего списка
        comboBoxView->setContextMenuPolicy(Qt::CustomContextMenu);

        connect(comboBoxView, &QAbstractItemView::customContextMenuRequested, this, [this](const QPoint &pos) {
            QAbstractItemView* view = ui->configComboBox->view();
            QModelIndex index = view->indexAt(pos);

            if (!index.isValid()) return;

            // Извлекаем ID плагина, который мы сохраняли в UserRole (или currentData)
            int id = index.data(Qt::UserRole).toInt();

            // Запрещаем удалять служебные элементы (например, заглушки или кнопку "Добавить")
            if (id <= 0) return;

            QString pluginName = index.data(Qt::DisplayRole).toString();

            // Создаем контекстное меню
            QMenu contextMenu(this);

            // Подкрашиваем иконку мусорки под текущую тему (используем твою лямбду из updateInterfaceIcons, если она доступна)
            // Если лямбда недоступна глобально, просто берем исходный SVG:
            QIcon trashIcon(":/res/trash_google.svg");

            QAction *deleteAction = contextMenu.addAction(trashIcon, QString("Удалить \"%1\"").arg(pluginName));

            // Показываем меню в точке клика
            QAction *selectedAction = contextMenu.exec(view->mapToGlobal(pos));

            if (selectedAction == deleteAction) {
                // Скрываем сам комбобокс, чтобы QMessageBox не баговал с фокусом
                ui->configComboBox->hidePopup();

                // Вызываем подтверждение удаления
                auto res = QMessageBox::question(this, "Удаление плагина",
                                                 QString("Вы уверены, что хотите полностью удалить плагин \"%1\" и все его файлы?").arg(pluginName),
                                                 QMessageBox::Yes | QMessageBox::No);

                if (res == QMessageBox::Yes) {
                    // Вызываем удаление через твой PluginManager
                    m_pluginManager->removeConfig(id);

                    logMessage("Плагин успешно удален: " + pluginName, false);

                    // Обновляем комбобокс (вызываем твою функцию)
                    updateConfigList();

                    // Сбрасываем выбор на самый первый элемент
                    ui->configComboBox->setCurrentIndex(0);
                }
            }
        });
    }
}

void MainWindow::setupConnections()
{
    connect(m_watcher, &QFileSystemWatcher::directoryChanged, this, &MainWindow::onDirectoryChanged);

    // Когда таймер дотикает (прошло 500мс после последнего изменения в папках)
    connect(m_watchdogTimer, &QTimer::timeout, this, [this]() {
        if (m_needsPluginUpdate) {
            m_pluginManager->refreshPlugins();
            updateConfigList();
            m_needsPluginUpdate = false; // сбрасываем флаг
        }

        if (m_needsXlsxUpdate) {
            updateXlsxList();
            m_needsXlsxUpdate = false; // сбрасываем флаг
        }
    });

    // --- 4. СОБЫТИЯ И ИНТЕРФЕЙС ---
    connect(ui->configComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::onConfigSelected);
    connect(ui->browseXlsxButton, &QPushButton::clicked, this, &MainWindow::onBrowseXlsxClicked);
    connect(ui->openFolderButton, &QPushButton::clicked, this, &MainWindow::onOpenFolderClicked);
    connect(ui->startButton, &QPushButton::clicked, this, &MainWindow::onStartClicked);
    connect(ui->settingsButton, &QPushButton::clicked, this, &MainWindow::onSettingsClicked);
    connect(m_pluginManager, &PluginManager::pluginFinished, this, &MainWindow::onPluginFinished);
    connect(ui->openXlsxFolderButton, &QPushButton::clicked, this, &MainWindow::onOpenXlsxFolderClicked);
    connect(ui->refreshXlsxButton, &QPushButton::clicked, this, &MainWindow::updateXlsxList);

    // --- 5. ПОДПИСКА НА СИГНАЛЫ ДВИЖКА (через менеджер) ---
    connect(m_pluginManager, &PluginManager::pluginLogReceived, this, &MainWindow::onPluginLogReceived);
    connect(m_pluginManager, &PluginManager::pluginReadyChanged, this, &MainWindow::onPluginReadyChanged);
}

// Вспомогательный метод для очистки стилей
void MainWindow::clearHoverStyles()
{
    // Возвращаем дефолтные стили (можешь настроить под свой темный интерфейс)
    ui->configComboBox->setStyleSheet("");
    ui->xlsxList->setStyleSheet("");
}

// 1. Файл занесли над окном
void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction(); // Разрешаем отслеживание
    } else {
        event->ignore();
    }
}

// 2. Файл перемещают внутри окна (ПИШЕМ ТУТ ХОВЕР-АНИМАЦИЮ)
void MainWindow::dragMoveEvent(QDragMoveEvent *event)
{
    if (!event->mimeData()->hasUrls() || event->mimeData()->urls().isEmpty()) {
        event->ignore();
        return;
    }

    QPoint pos = event->position().toPoint();
    QString filePath = event->mimeData()->urls().first().toLocalFile();
    QFileInfo fi(filePath);
    QString ext = fi.suffix().toLower();
    QString compExt = fi.completeSuffix().toLower();

    bool isValidPluginArchive = (ext == "zip" || ext == "tar" || compExt.endsWith("tar.gz") || compExt.endsWith("tgz"));
    bool isValidExcelFile = (ext == "xlsx" || ext == "xls");

    // Зона плагинов принимает ТОЛЬКО валидные архивы
    if (ui->configComboBox->geometry().contains(pos)) {
        if (isValidPluginArchive) {
            ui->configComboBox->setStyleSheet("border: 2px solid #2ecc71; background-color: #1e272e;");
            ui->xlsxList->setStyleSheet("");
            event->acceptProposedAction();
            return;
        }
    }
    // Зона XLSX принимает эксельки ИЛИ архивы, из которых мы их вытащим
    else if (ui->xlsxList->geometry().contains(pos)) {
        if (isValidExcelFile || isValidPluginArchive) {
            ui->xlsxList->setStyleSheet("border: 2px solid #3498db; background-color: #1e272e;");
            ui->configComboBox->setStyleSheet("");
            event->acceptProposedAction();
            return;
        }
    }

    // Если файл не подходит — тушим рамки и шлем его лесом
    clearHoverStyles();
    event->ignore(); // Юзер увидит значок запрета на дроп
}

// 3. Юзер увёл мышь из окна, так и не сбросив файл
void MainWindow::dragLeaveEvent(QDragLeaveEvent *event)
{
    clearHoverStyles();
    event->accept();
}

// 4. Юзер отпустил кнопку мыши (ДРОП)
void MainWindow::dropEvent(QDropEvent *event)
{
    clearHoverStyles(); // Гасим рамки подсветки

    if (!event->mimeData()->hasUrls() || event->mimeData()->urls().isEmpty()) {
        event->ignore();
        return;
    }

    QPoint pos = event->position().toPoint();
    QList<QUrl> urls = event->mimeData()->urls();
    QString filePath = urls.first().toLocalFile();
    QFileInfo fi(filePath);
    QString ext = fi.suffix().toLower();
    QString compExt = fi.completeSuffix().toLower(); // Для .tar.gz

    // Флаг: является ли файл поддерживаемым типом архива
    bool isArchive = (ext == "zip" || ext == "tar" || compExt.endsWith("tar.gz") || compExt.endsWith("tgz"));

    // ========================================================================
    // ЗОНА ПЛАГИНОВ (configComboBox)
    // ========================================================================
    if (ui->configComboBox->geometry().contains(pos)) {
        if (!isArchive) {
            logMessage("Ошибка: В зону плагинов можно сбрасывать только архивы (ZIP, TAR, TAR.GZ)!", true);
            QMessageBox::critical(this, "Неверный формат", "Сюда можно сбрасывать только файлы плагинов в формате архивов (.zip, .tar, .tar.gz)!");
            event->ignore();
            return;
        }

        // Вызываем обновленный метод менеджера плагинов
        QPair<int, QString> result = m_pluginManager->addConfigFromArchive(filePath);
        int newId = result.first;
        QString errorMsg = result.second;

        if (newId != -1) {
            logMessage("Плагин успешно добавлен! ID: " + QString::number(newId), false);
            updateConfigList();

            int newIndex = ui->configComboBox->findData(newId);
            if (newIndex != -1) ui->configComboBox->setCurrentIndex(newIndex);
        } else {
            logMessage("Ошибка добавления плагина: " + errorMsg, true);
            QMessageBox::critical(this, "Ошибка добавления плагина", errorMsg);
            ui->configComboBox->setCurrentIndex(0);
        }

        event->acceptProposedAction();
        return;
    }

    // ========================================================================
    // ЗОНА XLSX ТАБЛИЦ (xlsxList)
    // ========================================================================
    if (ui->xlsxList->geometry().contains(pos)) {
        QSettings settings("FinWizard", "Settings");
        QString inputFolder = settings.value("inputFolder",
                                             QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/FinWizard_Input").toString();

        QDir inputDir(inputFolder);
        if (!inputDir.exists() && !inputDir.mkpath(".")) {
            event->ignore();
            return;
        }

        bool atLeastOneProcessed = false;

        for (const QUrl &url : urls) {
            QString curPath = url.toLocalFile();
            QFileInfo curFi(curPath);
            QString curExt = curFi.suffix().toLower();
            QString curCompExt = curFi.completeSuffix().toLower();

            // Если кинули чистый Excel
            if (curExt == "xlsx" || curExt == "xls") {
                QString dest = inputFolder + "/" + curFi.fileName();
                if (QFile::copy(curPath, dest)) {
                    logMessage("Скопирован: " + curFi.fileName(), false);
                    atLeastOneProcessed = true;
                } else {
                    logMessage("Ошибка копирования: " + curFi.fileName(), true);
                }
            }
            // Если кинули архив с таблицами (ZIP или TAR)
            else if (curExt == "zip" || curExt == "tar" || curCompExt.endsWith("tar.gz") || curCompExt.endsWith("tgz")) {
                QTemporaryDir tempDir;
                if (!tempDir.isValid()) continue;

                // Юзаем наш ArchiveManager! Он атомарен и проверит целостность перед распаковкой
                if (!ArchiveManager::extractArchive(curPath, tempDir.path())) {
                    logMessage(QString("Ошибка: Архив %1 поврежден или пуст!").arg(curFi.fileName()), true);
                    QMessageBox::critical(this, "Ошибка архива", QString("Не удалось извлечь архив %1. Он поврежден.").arg(curFi.fileName()));
                    continue;
                }

                // Вытаскиваем эксельки из временной папки
                QDirIterator it(tempDir.path(), {"*.xlsx", "*.xls"}, QDir::Files, QDirIterator::Subdirectories);
                while (it.hasNext()) {
                    QString extractedFilePath = it.next();
                    if (extractedFilePath.contains("__MACOSX")) continue; // Игнорим мусор macOS

                    QFileInfo extFi(extractedFilePath);
                    QString dest = inputFolder + "/" + extFi.fileName();

                    if (QFile::exists(dest)) {
                        QFile::remove(dest);
                    }

                    if (QFile::copy(extractedFilePath, dest)) {
                        logMessage("Извлечено из архива: " + extFi.fileName(), false);
                        atLeastOneProcessed = true;
                    }
                }
            }
            // Если закинули левый файл (картинку, музыку, софт)
            else {
                logMessage(QString("Файл отклонен (не таблица/архив): %1").arg(curFi.fileName()), true);
                QMessageBox::warning(this, "Неподдерживаемый файл",
                                     QString("Файл %1 не является таблицей Excel или поддерживаемым архивом!").arg(curFi.fileName()));
            }
        }

        if (atLeastOneProcessed) {
            updateXlsxList();
        }

        event->acceptProposedAction();
        return;
    }

    event->ignore();
}

void MainWindow::showXlsxContextMenu(const QPoint &pos)
{
    QListWidgetItem *item = ui->xlsxList->itemAt(pos);
    QMenu contextMenu(this);

    // Вытаскиваем путь один раз и с безопасным дефолтом
    QSettings settings("FinWizard", "Settings");
    QString defaultInput = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/FinWizard_Input";
    QString inputFolder = settings.value("inputFolder", defaultInput).toString();
    QDir dir(inputFolder);

    if (item) {
        QAction *copyAction = contextMenu.addAction("Копировать имя");
        QAction *openAction = contextMenu.addAction("Открыть файл");
        contextMenu.addSeparator();
        QAction *deleteAction = contextMenu.addAction("Удалить файл");

        QAction *selectedAction = contextMenu.exec(ui->xlsxList->mapToGlobal(pos));

        if (selectedAction == copyAction) {
            QApplication::clipboard()->setText(item->text());
            logMessage("Имя файла скопировано в буфер", false);
        }
        else if (selectedAction == openAction) {
            QString fullPath = dir.absoluteFilePath(item->text());
            QDesktopServices::openUrl(QUrl::fromLocalFile(fullPath));
        }
        else if (selectedAction == deleteAction) {
            auto res = QMessageBox::question(this, "Удаление", "Удалить файл " + item->text() + "?",
                                             QMessageBox::Yes | QMessageBox::No);
            if (res == QMessageBox::Yes) {
                QFile file(dir.absoluteFilePath(item->text()));

                if (file.remove()) {
                    logMessage("Файл удален: " + item->text(), false);
                    updateXlsxList();
                } else {
                    logMessage("Не удалось удалить файл: " + file.errorString(), true);
                }
            }
        }
    }
    else {
        QAction *addFilesAction = contextMenu.addAction("Добавить файлы...");
        QAction *refreshAction = contextMenu.addAction("Обновить список");

        QAction *selectedAction = contextMenu.exec(ui->xlsxList->mapToGlobal(pos));

        if (selectedAction == addFilesAction) {
            onBrowseXlsxClicked();
        } else if (selectedAction == refreshAction) {
            updateXlsxList();
        }
    }
}

void MainWindow::onDeleteConfigClicked()
{
    int selectedId = ui->configComboBox->currentData().toInt();

    // Запрещаем удалять заглушку или пункт "Добавить..."
    if (selectedId <= 0) {
        QMessageBox::warning(this, "Предупреждение", "Выберите валидный плагин для удаления.");
        return;
    }

    // Проверяем, не заблокирован ли интерфейс (работает ли плагин)
    if (!ui->startButton->isEnabled()) {
        QMessageBox::warning(this, "Ошибка", "Нельзя удалить плагин во время его выполнения или сборки окружения.");
        return;
    }

    QString configName = ui->configComboBox->currentText();

    auto res = QMessageBox::question(this, "Удаление плагина",
                                     QString("Вы уверены, что хотите полностью удалить плагин '%1' и все его файлы?").arg(configName),
                                     QMessageBox::Yes | QMessageBox::No);

    if (res == QMessageBox::Yes) {
        // Вызываем удаление через менеджер
        m_pluginManager->removeConfig(selectedId);

        logMessage("Плагин успешно удален: " + configName, false);

        // Обновляем комбобокс
        updateConfigList();

        // Сбрасываем на дефолтный пункт "Выберите конфиг..."
        ui->configComboBox->setCurrentIndex(0);
    }
}

void MainWindow::onStartClicked()
{
    QSettings settings("FinWizard", "Settings");
    QDir inputDir(settings.value("inputFolder", QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)).toString());

    // 1. Проверяем, выбран ли конфиг
    if (ui->configComboBox->currentIndex() <= 0) {
        QMessageBox::warning(this, "Ошибка", "Сначала выберите конфиг из списка");
        return;
    }

    int id = ui->configComboBox->currentData().toInt();

    // 2. Получаем input файлы
    if (!inputDir.exists()) {
        logMessage(QString("Папка input не существует: %1").arg(inputDir.absolutePath()), true);
        return;
    }

    QStringList inputXlsxFiles = inputDir.entryList({"*.xlsx", "*.xls"}, QDir::Files | QDir::NoDotAndDotDot);
    if (inputXlsxFiles.isEmpty()) {
        QMessageBox::warning(this, "Ошибка", "В папке input нет XLSX-файлов");
        logMessage("СТАРТ: файлы xlsx не найдены", true);
        return;
    }

    // 3. Собираем параметры
    QStringList fullPaths;
    for (const QString &fileName : inputXlsxFiles) {
        fullPaths << inputDir.absoluteFilePath(fileName);
    }

    QString outputFolder = settings.value("outputFolder", QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/FinWizard_Results").toString();
    QDir outputDir(outputFolder);
    if (!outputDir.exists() && !outputDir.mkpath(".")) {
        QMessageBox::critical(this, "Ошибка", "Не удалось создать папку результатов:\n" + outputFolder);
        return;
    }

    // Просто берем имя файла из GUI. Больше никаких кэшей и проверок на автостарт!
    QString desiredFileName = ui->fileName->text().trimmed();
    if (desiredFileName.isEmpty()) {
        QMessageBox::critical(this, "Ошибка", "Введите название файла!");
        return;
    }

    // Корректная проверка без дублирования расширений (.xlsx.xlsx)
    if (!desiredFileName.endsWith(".xlsx", Qt::CaseInsensitive) && !desiredFileName.endsWith(".xls", Qt::CaseInsensitive)) {
        desiredFileName += ".xlsx";
    }

    QString outputFile = outputDir.absoluteFilePath(desiredFileName);

    // Проверяем существование файла
    QFile file(outputFile);
    if (file.exists()) {
        QMessageBox::critical(this, "Ошибка", "Файл с таким именем уже существует!");
        return;
    }

    QVariantMap params;
    params["xlsxFiles"] = fullPaths;
    params["outputFolder"] = outputFolder;
    params["outputFile"] = outputFile;
    params["desiredFileName"] = desiredFileName;

    // 4. ЗАПУСК!
    logMessage("Запуск конфига ID " + QString::number(id) + "...", false);

    // Блокируем интерфейс сразу
    ui->startButton->setEnabled(false);
    ui->configComboBox->setEnabled(false);
    ui->startButton->setText("⚙️ Проверка...");

    // Форсируем мгновенную перерисовку кнопки, чтобы текст обновился на экране.
    // При этом очередь событий не прокручивается, и повторные клики не вызовут Race Condition.
    ui->startButton->repaint();

    QVariantMap result = m_pluginManager->runConfig(id, params);

    // 5. ОБРАБОТКА РЕЗУЛЬТАТА
    if (result.value("success").toBool()) {
        // СЛУЧАЙ 1: Скрипт (или DLL) выполнился мгновенно
        ui->startButton->setEnabled(true);
        ui->configComboBox->setEnabled(true);
        ui->startButton->setText("🚀 СТАРТ");

        QString msg = result.value("message").toString();
        QString outPath = result.value("outputPath").toString();

        logMessage("Успех: " + msg, false);

        if (!outPath.isEmpty()) {
            logMessage("Результат сохранён: " + outPath, false);
            if (settings.value("autoOpenResultFolder", true).toBool()) {
                QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(outPath).absolutePath()));
            }
        }

        // Очищаем поле ввода, так как всё уже сделано
        ui->fileName->clear();
    }
    else if (result.value("isInitializing").toBool()) {
        // СЛУЧАЙ 2: Движок ушел разворачивать venv и ставить pip
        QString msg = result.value("error").toString();
        logMessage(msg, false);

        // Оставляем кнопки заблокированными! Текст меняем на статус ожидания
        ui->startButton->setText("⏳ Установка библиотек...");

        // Поле ui->fileName НЕ ОЧИЩАЕМ. Функция просто завершает работу.
        // Движок сам перехватит управление и выполнит плагин по окончании pip.
    }
    else {
        // СЛУЧАЙ 3: Обычная ошибка (не смогли загрузить плагин, упал EXE и т.д.)
        ui->startButton->setEnabled(true);
        ui->configComboBox->setEnabled(true);
        ui->startButton->setText("🚀 СТАРТ");

        QString error = result.value("error").toString();
        logMessage("Ошибка: " + error, true);
        QMessageBox::critical(this, "Ошибка выполнения", error);
    }
}

void MainWindow::onBrowseXlsxClicked()
{
    QSettings settings("FinWizard", "Settings");
    QString inputFolder = settings.value("inputFolder", QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/FinWizard_Input").toString();

    QDir inputDir(inputFolder);
    if (!inputDir.exists() && !inputDir.mkpath(".")) return;

    // 1. Расширяем строку фильтра файлов для диалогового окна
    QStringList selectedFiles = QFileDialog::getOpenFileNames(
        this, "Выберите XLSX-файлы или Архивы таблиц",
        settings.value("lastInputFolder").toString(),
        "Поддерживаемые файлы (*.xlsx *.xls *.zip *.tar *.tar.gz *.tgz)"
        );

    if (selectedFiles.isEmpty()) return;

    settings.setValue("lastInputFolder", QFileInfo(selectedFiles.first()).absolutePath());

    bool atLeastOneProcessed = false;

    for (const QString &filePath : selectedFiles) {
        QFileInfo fi(filePath);
        QString ext = fi.suffix().toLower();
        QString compExt = fi.completeSuffix().toLower(); // Для отлова комбо-вариантов (.tar.gz)

        // А) Если это обычная одиночная таблица Excel
        if (ext == "xlsx" || ext == "xls") {
            QString dest = inputFolder + "/" + fi.fileName();

            if (QFile::exists(dest)) {
                QFile::remove(dest);
            }

            if (QFile::copy(filePath, dest)) {
                logMessage("Скопирован: " + fi.fileName(), false);
                atLeastOneProcessed = true;
            } else {
                logMessage("Ошибка копирования (проверьте доступ): " + fi.fileName(), true);
            }
        }
        // Б) Универсальная обработка ЛЮБЫХ архивов (ZIP, TAR, TAR.GZ, TGZ)
        else if (ext == "zip" || ext == "tar" || compExt.endsWith("tar.gz") || compExt.endsWith("tgz")) {
            QTemporaryDir tempDir;
            if (!tempDir.isValid()) continue;

            // Наш атомарный ArchiveManager делает сухую валидацию, а потом распаковывает
            if (!ArchiveManager::extractArchive(filePath, tempDir.path())) {
                logMessage(QString("Ошибка: Архив %1 поврежден, пуст или не поддерживается!").arg(fi.fileName()), true);
                QMessageBox::critical(this, "Ошибка архива", QString("Не удалось извлечь архив %1. Он поврежден.").arg(fi.fileName()));
                continue;
            }

            // Ищем эксельки рекурсивно внутри временной папки
            QDirIterator it(tempDir.path(), {"*.xlsx", "*.xls"}, QDir::Files, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                QString extractedFilePath = it.next();

                if (extractedFilePath.contains("__MACOSX")) continue; // Игнорируем мусор от macOS

                QFileInfo extFi(extractedFilePath);
                QString dest = inputFolder + "/" + extFi.fileName();

                if (QFile::exists(dest)) {
                    QFile::remove(dest);
                }

                if (QFile::copy(extractedFilePath, dest)) {
                    logMessage("Извлечено из архива: " + extFi.fileName(), false);
                    atLeastOneProcessed = true;
                }
            }
        }
    }

    if (atLeastOneProcessed) {
        updateXlsxList();
    }
}

void MainWindow::onSettingsClicked()
{
    QDialog dlg(this);
    dlg.setWindowTitle("Настройки");
    dlg.resize(650, 420);

    QVBoxLayout *mainLayout = new QVBoxLayout(&dlg);
    QSettings settings("FinWizard", "Settings");

    QCheckBox *autoOpenCheck = new QCheckBox("Автоматически открывать папку с результатами", &dlg);
    autoOpenCheck->setChecked(settings.value("autoOpenResultFolder", true).toBool());
    mainLayout->addWidget(autoOpenCheck);

    // --- ОПРЕДЕЛЕНИЕ КОРРЕКТНЫХ ДЕФОЛТНЫХ ПУТЕЙ БЕЗ КИРИЛЛИЦЫ ДЛЯ WINDOWS ---
    QString defaultCachePath;
    QString defaultInputPath;

#if defined(Q_OS_WIN)
    // Уходим от C:/Users/Кириллица/AppData во избежание проблем с venv и кодировками
    defaultCachePath = "C:/ProgramData/FinWizard/plugins-cache";
    defaultInputPath = "C:/ProgramData/FinWizard/Input";
#else
    // Для Linux и остальных ОС оставляем стандартные пути в домашней папке
    defaultCachePath = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/FinWizard/plugins-cache";
    defaultInputPath = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/FinWizard_Input";
#endif

    // Используем лямбду, передавая dlg как родителя для всех создаваемых элементов
    auto createPathRow = [&](const QString &label, const QString &key, const QString &def) {
        QHBoxLayout *row = new QHBoxLayout(); // Перейдет под управление mainLayout при addLayout
        row->addWidget(new QLabel(label, &dlg));
        QLineEdit *edit = new QLineEdit(settings.value(key, def).toString(), &dlg);
        QPushButton *btn = new QPushButton("Обзор", &dlg);

        connect(btn, &QPushButton::clicked, [&dlg, edit]() {
            QString path = QFileDialog::getExistingDirectory(&dlg, "Выберите папку", edit->text());
            if (!path.isEmpty()) edit->setText(path);
        });

        row->addWidget(edit);
        row->addWidget(btn);
        mainLayout->addLayout(row);
        return edit;
    };

    QLineEdit *resultEdit = createPathRow("Папка для результатов:", "outputFolder",
                                          QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/FinWizard_Results");

    // Подставляем кроссплатформенные дефолтные значения, настроенные под Windows
    QLineEdit *inputEdit = createPathRow("Папка для входных файлов:", "inputFolder", defaultInputPath);
    QLineEdit *cacheEdit = createPathRow("Папка кэша плагинов:", "cache/path", defaultCachePath);

    QPushButton *saveBtn = new QPushButton("Сохранить", &dlg);
    connect(saveBtn, &QPushButton::clicked, [&]() {
        settings.setValue("autoOpenResultFolder", autoOpenCheck->isChecked());
        settings.setValue("outputFolder", resultEdit->text());
        settings.setValue("inputFolder", inputEdit->text());
        settings.setValue("cache/path", cacheEdit->text());

        m_pluginManager->setCacheBasePath(cacheEdit->text());

        if (!m_watcher->directories().isEmpty()) {
            m_watcher->removePaths(m_watcher->directories());
        }
        m_watcher->addPath(inputEdit->text());
        m_watcher->addPath(cacheEdit->text());

        updateConfigList();
        updateXlsxList();
        logMessage("Настройки сохранены", false);
        dlg.accept();
    });

    QPushButton *cancelBtn = new QPushButton("Отмена", &dlg);
    connect(cancelBtn, &QPushButton::clicked, &dlg, &QDialog::reject);

    QHBoxLayout *btnLayout = new QHBoxLayout(); // Перейдет под управление mainLayout
    btnLayout->addStretch();
    btnLayout->addWidget(cancelBtn);
    btnLayout->addWidget(saveBtn);

    mainLayout->addStretch();
    mainLayout->addLayout(btnLayout);

    dlg.exec();
}

void MainWindow::onDirectoryChanged(const QString &path)
{
    QSettings settings("FinWizard", "Settings");

    // Получаем текущие пути из настроек (очищаем их от лишних слешей для точного сравнения)
    QString configsPath = QDir::cleanPath(settings.value("cache/path").toString());
    QString inputPath = QDir::cleanPath(settings.value("inputFolder").toString());
    QString changedPath = QDir::cleanPath(path);

    if (changedPath == configsPath) {
        m_needsPluginUpdate = true;
    }
    else if (changedPath == inputPath) {
        m_needsXlsxUpdate = true;
    }

    // Запускаем/перезапускаем таймер.
    // Пока файлы копируются (идет спам событий), таймер будет постоянно сбрасываться.
    // Как только копирование завершится, таймер отсчитает 500мс и обновит UI.
    m_watchdogTimer->start();
}

void MainWindow::updateXlsxList()
{
    ui->xlsxList->clear();
    QSettings settings("FinWizard", "Settings");

    // Получаем правильный дефолтный путь, точно так же, как в конструкторе
    QString defaultInput = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/FinWizard_Input";
    QString inputFolderPath = settings.value("inputFolder", defaultInput).toString();

    QDir inputDir(inputFolderPath);

    // Добавляем фильтр масок (включая .xls) + делаем поиск регистронезависимым
    QStringList filters = {"*.xlsx", "*.xls"};

    // entryList теперь вернет и мелкие, и капсовые расширения (.XLSX)
    QStringList files = inputDir.entryList(filters, QDir::Files, QDir::Name | QDir::IgnoreCase);

    ui->xlsxList->addItems(files);
}

void MainWindow::updateConfigList()
{
    // Запоминаем текущий выбранный ID
    int savedId = ui->configComboBox->currentData().toInt();

    ui->configComboBox->blockSignals(true);
    ui->configComboBox->clear();

    // 1. Возвращаем дефолтный пункт-заглушку
    ui->configComboBox->addItem("Выберите конфиг из списка...", 0);

    // Достаем нормальную карту ID -> Имя
    QMap<int, QString> configs = m_pluginManager->getAvailableConfigs();

    // По умолчанию сфокусируемся на заглушке (индекс 0)
    int indexToSelect = 0;

    // Итерируемся по карте — теперь ID и Имя намертво связаны
    for (auto it = configs.constBegin(); it != configs.constEnd(); ++it) {
        int id = it.key();
        QString name = it.value();

        ui->configComboBox->addItem(name, id);

        // Если сохраненный ID валидный (не заглушка и не добавление) и совпал — выберем его
        if (savedId > 0 && id == savedId) {
            indexToSelect = ui->configComboBox->count() - 1;
        }
    }

    // Добавляем пункт создания
    ui->configComboBox->addItem("Добавить конфиг...", -999);

    // СТИЛИЗУЕМ СПЕЦИАЛЬНЫЕ ПУНКТЫ (Заглушку и Добавление)
    QStandardItemModel *model = qobject_cast<QStandardItemModel*>(ui->configComboBox->model());
    if (model) {
        int rowCount = model->rowCount();

        // Проверяем текущую тему системы через цвет окна
        bool isDark = ui->centralwidget->palette().color(QPalette::Window).value() < 128;

        // 1. Красим заглушку (0-я строка) в адаптивный серый
        QStandardItem *placeholderItem = model->item(0);
        if (placeholderItem) {
            QColor grayColor = isDark ? QColor("#aaaaaa") : QColor("#666666");
            placeholderItem->setForeground(grayColor);
            QFont font = placeholderItem->font();
            font.setItalic(true);
            placeholderItem->setFont(font);
        }

        // 2. Красим пункт добавления (последняя строка) в адаптивный зеленый[cite: 1]
        QStandardItem *addItem = model->item(rowCount - 1);
        if (addItem) {
            QColor greenColor = isDark ? QColor("#2ecc71") : QColor("#1e8449"); //[cite: 1]
            addItem->setForeground(greenColor);
            QFont font = addItem->font();
            font.setItalic(true);
            addItem->setFont(font);
        }
    }

    // Ставим корректный индекс
    ui->configComboBox->setCurrentIndex(indexToSelect);
    ui->configComboBox->blockSignals(false);

    // ВАЖНО: Если мы сбросили на заглушку (индекс 0), нужно вручную очистить
    // превью интерфейса, чтобы там не висел старый текст
    if (indexToSelect == 0) {
        // Вызови здесь свой метод очистки превью, например:
        // updatePreviewFields(QMap<QString, QString>());
        // или ui->pluginNameLabel->setText("Здесь будет название...");
    }
}

void MainWindow::onConfigSelected(int index)
{
    int selectedId = ui->configComboBox->itemData(index).toInt();

    if (selectedId == -999) {
        QMetaObject::invokeMethod(this, "onAddConfigClicked", Qt::QueuedConnection);
        ui->configComboBox->setCurrentIndex(0);
        return;
    }

    if (selectedId <= 0) {
        ui->startButton->setEnabled(false);
        return;
    }

    // Мы больше не тащим IConfig и CachedConfig сюда. Берем имя прямо из ComboBox!
    QString configName = ui->configComboBox->itemText(index);
    logMessage(QString("Выбран конфиг: %1").arg(configName), false);

    updateConfigPreview(selectedId);
    ui->startButton->setEnabled(true);
}

void MainWindow::updateConfigPreview(int configId)
{
    ui->configIcon->clear();
    ui->configName->setText("Не выбрано");
    ui->configDescription->clear();

    QMap<QString, QString> preview = m_pluginManager->getConfigPreview(configId);
    if (preview.isEmpty()) return;

    if (!preview["name"].isEmpty()) ui->configName->setText(preview["name"]);
    if (!preview["description"].isEmpty()) ui->configDescription->setText(preview["description"]);

    if (!preview["icon"].isEmpty()) {
        QPixmap pixmap(preview["icon"]);
        if (!pixmap.isNull()) {
            QSize iconSize(64, 64);
            ui->configIcon->setPixmap(pixmap.scaled(iconSize, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        } else {
            qDebug() << "Не удалось загрузить картинку по пути:" << preview["icon"];
        }
    }
}

void MainWindow::onAddConfigClicked()
{
    // Расширяем фильтр диалогового окна на все наши форматы
    QString archivePath = QFileDialog::getOpenFileName(this, "Выберите архив плагина",
                                                       QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation),
                                                       "Архивы плагинов (*.zip *.tar *.tar.gz *.tgz)");
    if (archivePath.isEmpty()) return;

    // Переключаем на метод addConfigFromArchive
    QPair<int, QString> result = m_pluginManager->addConfigFromArchive(archivePath);

    int newId = result.first;
    QString errorMsg = result.second;

    if (newId != -1) {
        logMessage("Плагин успешно добавлен! ID: " + QString::number(newId), false);
        updateConfigList();

        int newIndex = ui->configComboBox->findData(newId);
        if (newIndex != -1) ui->configComboBox->setCurrentIndex(newIndex);
    } else {
        logMessage("Ошибка добавления плагина: " + errorMsg, true);
        QMessageBox::critical(this, "Ошибка добавления плагина", errorMsg);
        ui->configComboBox->setCurrentIndex(0);
    }
}

void MainWindow::logMessage(const QString &message, bool isError)
{
    QString time = QDateTime::currentDateTime().toString("hh:mm:ss");
    QString prefix = isError ? "[ОШИБКА]" : "[INFO]";

    // Динамически определяем, темный ли сейчас фон у лога в системе
    QColor bgColor = ui->logTextEdit->palette().color(QPalette::Base);
    bool isDark = bgColor.value() < 128;

    // Подбираем контрастные цвета
    QString color;
    if (isError) {
        color = isDark ? "#ff6b6b" : "#c0392b";
    } else {
        color = isDark ? "#2ecc71" : "#1e8449";
    }

    // Фикс ошибки: используем append() вместо appendHtml()
    ui->logTextEdit->append(QString("%1 %2 <font color=\"%3\"><b>%4</b></font>")
                                .arg(time, prefix, color, message.toHtmlEscaped()));

    ui->logTextEdit->ensureCursorVisible();
}

void MainWindow::onOpenFolderClicked()
{
    QSettings settings("FinWizard", "Settings");
    QDesktopServices::openUrl(QUrl::fromLocalFile(settings.value("cache/path").toString()));
}

// Сюда прилетает живой выхлоп от pip install
void MainWindow::onPluginLogReceived(int id, const QString &text)
{
    // Выводим в консоль/лог-панель приложения
    logMessage(QString("[Зависимости ID %1]: %2").arg(id).arg(text), false);

    // Выдергиваем последнюю строчку, чтобы показать прогресс прямо на кнопке
    // Убираем лишний мусор, берем только первые 25 символов для компактности
    QString shortText = text.left(25);
    if (!shortText.isEmpty()) {
        ui->startButton->setText("⏳ " + shortText + "...");
    }
}

// Этот слот вызывается, когда pip завершил работу (успешно или с ошибкой)
void MainWindow::onPluginReadyChanged(int id, bool success)
{
    // Pip закончил работу -> возвращаем кнопкам исходное состояние
    ui->startButton->setEnabled(true);
    ui->configComboBox->setEnabled(true);
    ui->startButton->setText("🚀 СТАРТ");

    if (success) {
        logMessage("Окружение готово, плагин запущен в фоне движком!", false);
        ui->fileName->clear(); // Очищаем поле, так как движок сам всё запишет
    } else {
        logMessage("Критическая ошибка: Зависимости Python не установлены.", true);
        QMessageBox::critical(this, "Ошибка окружения", "Не удалось установить библиотеки.");
    }
}

void MainWindow::onPluginFinished(int id, bool success, const QString &message, const QString &outputPath)
{
    // Возвращаем UI в рабочее состояние
    ui->startButton->setEnabled(true);
    ui->configComboBox->setEnabled(true);
    ui->startButton->setText("🚀 СТАРТ");

    if (success) {
        logMessage("Успех: " + message, false);

        if (!outputPath.isEmpty()) {
            logMessage("Результат сохранён: " + outputPath, false);

            QSettings settings("FinWizard", "Settings");
            if (settings.value("autoOpenResultFolder", true).toBool()) {
                QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(outputPath).absolutePath()));
            }
        }
        ui->fileName->clear();
    } else {
        logMessage("Ошибка отложенного запуска: " + message, true);
        QMessageBox::critical(this, "Ошибка выполнения", message);
    }
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    // Если кнопка Старт выключена, значит что-то активно работает
    if (!ui->startButton->isEnabled()) {
        auto res = QMessageBox::question(this, "Выполнение плагина",
                                         "Плагин или установка зависимостей еще активны.\nВы уверены, что хотите принудительно закрыть программу?",
                                         QMessageBox::Yes | QMessageBox::No);

        if (res == QMessageBox::Yes) {
            event->accept();
        } else {
            event->ignore();
        }
    } else {
        event->accept();
    }
}

void MainWindow::changeEvent(QEvent *event)
{
    // Перехватываем изменение системной палитры (смена темы ОС)
    if (event->type() == QEvent::PaletteChange) {
        // Даем Qt обновить внутреннюю палитру приложения
        QMainWindow::changeEvent(event);

        // Перерисовываем наши кастомные элементы под новые цвета
        updateConfigList();
        logMessage("Системная тема изменилась. Интерфейс адаптирован.", false);
        return;
    }

    QMainWindow::changeEvent(event);
}

void MainWindow::onOpenXlsxFolderClicked()
{
    QSettings settings("FinWizard", "Settings");
    // Берем путь из настроек, если его нет — дефолтный Documents
    QString inputFolderPath = settings.value("inputFolder",
                                             QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)).toString();

        QDir inputDir(inputFolderPath);
        // Если папки почему-то нет физически, создадим её, чтобы проводник не ругался
        if (!inputDir.exists()) {
        if (!inputDir.mkpath(".")) {
            logMessage("Не удалось создать или открыть папку: " + inputFolderPath, true);
            return;
        }
    }

    // Открываем папку в системном файловом менеджере (Explorer / Dolphin / Nautilus)
    bool success = QDesktopServices::openUrl(QUrl::fromLocalFile(inputFolderPath));

    if (!success) {
        logMessage("Ошибка при открытии папки в проводнике: " + inputFolderPath, true);
    }
}

void MainWindow::updateInterfaceIcons()
{
    // Проверяем текущую тему (светлая/темная)
    bool isDark = ui->centralwidget->palette().color(QPalette::Window).value() < 128;

        // Если тема ТЕМНАЯ — оставляем иконки белыми.
        // Если СВЕТЛАЯ — превращаем их в темно-серый/черный (#2c3e50), чтобы они были контрастными.
        QColor iconColor = isDark ? QColor(Qt::white) : QColor("#2c3e50");

    auto getTintedIcon = [&](const QString &resourcePath) -> QIcon {
        QPixmap pixmap(resourcePath);
        if (pixmap.isNull()) return QIcon();

        QPainter painter(&pixmap);
        painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
        painter.fillRect(pixmap.rect(), iconColor);
        painter.end();
        return QIcon(pixmap);
    };

    // Перекрашиваем каждую кнопку из твоего .ui файла
    ui->openFolderButton->setIcon(getTintedIcon(":/res/folder_configs_google.svg"));
    ui->browseXlsxButton->setIcon(getTintedIcon(":/res/file_export_google.svg"));
    ui->openXlsxFolderButton->setIcon(getTintedIcon(":/res/folder_open_google.svg"));
    ui->refreshXlsxButton->setIcon(getTintedIcon(":/res/refresh_google.svg"));
    ui->settingsButton->setIcon(getTintedIcon(":/res/settings_google.svg"));
}
