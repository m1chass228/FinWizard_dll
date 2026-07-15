// src/gui/mainwindow.h
#pragma once

#include <QMainWindow>
#include "finwizard/pluginrepository.h"
#include "ui_mainwindow.h"
#include <QFileSystemWatcher>
#include <QSettings>

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

class PluginManager;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(PluginManager *pluginManager, QWidget *parent = nullptr);
    ~MainWindow();

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void changeEvent(QEvent *event) override;

private slots:
    void updateConfigList();                  // обновить список конфигов в configComboBox
    void updateXlsxList();                    // обновить список файлов в QlistWidget
    void onConfigSelected(int index);         // при выборе конфига в выпадающем списке
    void onBrowseXlsxClicked();               // кнопка "Обзор" для XLSX
    void onOpenFolderClicked();               // кнопка "Открыть папку с конфигами"
    void onAddConfigClicked();                // добавить конфиг
    void onStartClicked();                    // большая кнопка "СТАРТ"
    void onSettingsClicked();                 // кнопка "Настройки"
    void onOpenXlsxFolderClicked();           // кнопка открытия папки с XLSX
    void onDirectoryChanged(const QString &path); // если папки меняются
    void logMessage(const QString &msg, bool isError = false);
    void updateConfigPreview(int configId);   // картинка
    void showXlsxContextMenu(const QPoint &pos); // контекстное меню
    void onPluginLogReceived(int id, const QString &text);
    void onPluginReadyChanged(int id, bool success);
    void updateInterfaceIcons();
    void onDeleteConfigClicked();
    void onClearInputDirButton();

private:
    PluginManager *m_pluginManager;
    QString m_currentXlsxPath;                // путь к выбранному XLSX-файлу
    QSettings m_appSettings{"FinWizard", "Settings"}; // Единый экземпляр вместо пересоздания в каждом методе
    QFileSystemWatcher *m_watcher;
    QTimer *m_watchdogTimer;
    Ui::MainWindow *ui;

    void clearHoverStyles();

    bool m_needsPluginUpdate = false;
    bool m_needsXlsxUpdate = false;

    QString m_lastDesiredFileName;

    bool m_isWaitingForPip = false;
    CachedConfig m_delayedCfg;
    QVariantMap m_delayedParams;

    void onPluginFinished(int id, bool success, const QString &message, const QString &outputPath);

    void initSettingsAndPaths();
    void setupWidgets();
    void setupConnections();
    void setupCoreComponents();

    QString m_configsPath;
    QString m_inputPath;
};