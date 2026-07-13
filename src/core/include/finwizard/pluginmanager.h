#ifndef PLUGINMANAGER_H
#define PLUGINMANAGER_H

#include <QObject>
#include <QVariantMap>
#include <QSettings>
#include <QPair>
#include <QStringList>
#include <QMap>

#include "finwizard/pluginrepository.h"
#include "finwizard/pluginengine.h"

class PluginManager : public QObject
{
    Q_OBJECT
public:
    explicit PluginManager(QObject *parent = nullptr);
    ~PluginManager() override;

    // --- Основное API ---
    QPair<int, QString> addConfigFromArchive(const QString &archivePath);
    void removeConfig(int id);

    QVariantMap runConfig(int id, const QVariantMap &params);
    void unloadPlugin(int id);

    // --- Информация для UI (с гарантией потокобезопасности/валидации) ---
    const CachedConfig* getConfig(int id) const;
    QList<int> getAllConfigIds() const;
    QStringList getDisplayNames() const;
    QMap<int, QString> getAvailableConfigs() const;
    QMap<QString, QString> getConfigPreview(int id) const;

    // --- Управление кэшем ---
    QString getCacheBasePath() const;
    void setCacheBasePath(const QString &path);
    void refreshPlugins();

signals:
    // Сигнал передает ID плагина и строчку лога из pip/процесса
    void pluginLogReceived(int id, const QString &text);

    // Сигнал сообщает, успешно ли завершилась установка зависимостей
    void pluginReadyChanged(int id, bool ready);

    // Сигнал возврата результатов работы плагина (успех, сообщение, путь к итоговому XLSX)
    void pluginFinished(int id, bool success, const QString &message, const QString &outputPath);

    void infoLogRequested(const QString &text);

private:
    QSettings m_settings;
    PluginRepository m_repository;
    PluginEngine m_engine;
};

#endif // PLUGINMANAGER_H