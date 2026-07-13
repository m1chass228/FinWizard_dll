#ifndef PLUGINENGINE_H
#define PLUGINENGINE_H

#include <QVariantMap>
#include <QPluginLoader>
#include <map>
#include <memory>
#include <QProcess>
#include <QObject>

#include "finwizard/iconfig.h"
#include "finwizard/pluginrepository.h" // Нужен только ради структуры CachedConfig

class PluginEngine : public QObject
{
    Q_OBJECT

public:
    explicit PluginEngine(QObject *parent = nullptr);
    ~PluginEngine();

    // Загружает плагин в память
    bool loadPlugin(const CachedConfig &cfg);

    // Выгружает плагин из памяти
    void unloadPlugin(int id);

    // Запускает метод execute()
    QVariantMap runPlugin(const CachedConfig &cfg, const QVariantMap &params);

    bool isWaitingForPip() const { return m_isWaitingForPip; }

signals:
    void pipLogReady(int id, const QString &text);
    void pipFinished(int id, bool success);
    void pluginFinished(int id, bool success, const QString &message, const QString &outputPath);

private:
    bool prepareDependencies(const QString &cacheDir);

    bool setupPythonEnvironment(const CachedConfig &cfg);

    // Универсальный метод для запуска .exe, .py и вообще любых внешних скриптов
    QVariantMap runExternalProcess(const CachedConfig &cfg, const QVariantMap &params);

    std::map<int, std::unique_ptr<QPluginLoader>> m_loaders;
    std::map<int, IConfig*> m_activeConfigs;

    QProcess *m_pipProcess = nullptr;

    QString getPythonExecutable(const CachedConfig &cfg) const;

    bool m_isWaitingForPip = false;
    CachedConfig m_delayedCfg;
    QVariantMap m_delayedParams;
};

#endif // PLUGINENGINE_H
