#ifndef PLUGINMANAGER_H
#define PLUGINMANAGER_H

#include <QObject>
#include <QVariantMap>
#include <QSettings>

#include "finwizard/pluginrepository.h"
#include "finwizard/pluginengine.h"

class PluginManager : public QObject
{
    Q_OBJECT
public:
    explicit PluginManager(QObject *parent = nullptr);
    ~PluginManager() override;

    // --- Основное API ---
    QPair<int, QString> addConfigFromZip(const QString &zipPath);
    void removeConfig(int id);
    QVariantMap runConfig(int id, const QVariantMap &params);
    void unloadPlugin(int id);

    // --- Информация для UI ---
    QList<int> getAllConfigIds() const;
    QStringList getDisplayNames() const;
    QMap<int, QString> getAvailableConfigs() const;
    QMap<QString, QString> getConfigPreview(int id) const;

    // --- Управление кэшем ---
    QString getCacheBasePath() const;
    void setCacheBasePath(const QString &path);
    void refreshPlugins();

private:
    QSettings m_settings;
    PluginRepository m_repository;
    PluginEngine m_engine;

signals:
    // Сигнал передает ID плагина и строчку лога из pip
    void pluginLogReceived(int id, const QString &text);

    // Сигнал сообщает, успешно ли завершилась установка зависимостей
    void pluginReadyChanged(int id, bool ready);

    void pluginFinished(int id, bool success, const QString &message, const QString &outputPath);
};

#endif // PLUGINMANAGER_H
