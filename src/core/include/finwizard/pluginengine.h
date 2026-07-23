#ifndef PLUGINENGINE_H
#define PLUGINENGINE_H

#include <QVariantMap>
#include <QPluginLoader>
#include <map>
#include <memory>
#include <QProcess>
#include <QObject>
#include <functional>
#include <QJsonObject>

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

    // Занят ли движок выполнением внешнего процесса (.exe / .py) прямо сейчас.
    // UI должен блокировать повторный запуск, пока это true.
    bool isRunningExternalProcess() const { return m_execProcess != nullptr; }

    // Хук для запросов из RPC-моста (input.json/output.json — по-прежнему
    // основной контракт; это только для лёгких live-запросов вида get_db_data,
    // которые специфичны для приложения — движок сам ничего не знает о БД).
    // Handler получает (cfgId, action, params) и должен вернуть QVariantMap
    // с ключом "success" (bool) и либо "result", либо "error".
    // ВАЖНО: вызывается СИНХРОННО из обработчика readyReadStandardOutput,
    // пока процесс плагина заблокирован на readline() в ожидании ответа —
    // хендлер обязан быть быстрым, никакой долгой работы внутри.
    void setBridgeHandler(std::function<QVariantMap(int, const QString&, const QVariantMap&)> handler) {
        m_bridgeHandler = std::move(handler);
    }

    // Уменьшает счетчик использования каждого разделяемого слота, на который
    // ссылался этот плагин (через его shared_deps.json), и физически удаляет
    // слот с диска, если счетчик дошел до нуля — то есть больше НИ ОДИН
    // плагин его не использует. Вызывать ДО удаления cachePath плагина с
    // диска (иначе shared_deps.json уже не прочитать).
    void releasePluginDependencies(int pluginId, const QString &cachePath);

signals:
    void pipLogReady(int id, const QString &text);
    void pipFinished(int id, bool success);
    void pluginFinished(int id, bool success, const QString &message, const QString &outputPath);
    void infoLogRequested(const QString &text);
    void pluginLogRequested(int id, const QString &message);
    void pluginProgress(int id, int percent, const QString &text);

private:
    bool prepareDependencies(const QString &cacheDir);

    bool setupPythonEnvironment(const CachedConfig &cfg);

    bool startExternalProcessAsync(const CachedConfig &cfg, const QVariantMap &params, QString &startupError);

    bool isVenvValid(const QString &cachePath, const QString &currentBasePython) const;

    QString findBaseInterpreter() const;

    // === ОБЩИЙ ПУЛ ЗАВИСИМОСТЕЙ ===
    // Раньше каждый плагин ставил зависимости в СВОЮ py_deps — два плагина
    // с одинаковой "openpyxl==3.1.5" качали и ставили её дважды. Теперь
    // каждая строка зависимости (как есть, текстом) маппится в один и тот
    // же слот в sharedDepsRoot() — если он там уже установлен ЛЮБЫМ другим
    // плагином, pip для него вообще не запускается.
    QString sharedDepsRoot() const;
    QString depSlotSlug(const QString &depLine) const;
    QStringList readRequirementLines(const QString &reqPath) const;
    void writeSharedDepsManifest(const CachedConfig &cfg, const QStringList &depSpecs) const;
    QJsonObject readSharedDepsIndex() const;
    void writeSharedDepsIndex(const QJsonObject &index) const;
    bool installNextPendingDependency();

    // Очередь недостающих (в общем пуле) зависимостей для ТЕКУЩЕГО вызова
    // setupPythonEnvironment(). Один pip-процесс за раз (как и раньше),
    // просто теперь на каждую строку зависимости, а не на requirements.txt
    // целиком — очередь опустошается по одной, installNextPendingDependency()
    // сам себя вызывает на успехе, пока не останется ни одной.
    QStringList m_pendingSharedInstalls;
    CachedConfig m_pendingDepsCfg;
    int m_pendingDepsTotalAtStart = 0;

    std::map<int, std::unique_ptr<QPluginLoader>> m_loaders;
    std::map<int, IConfig*> m_activeConfigs;

    QProcess *m_pipProcess = nullptr;
    QProcess *m_execProcess = nullptr; // Плагин (.exe/.py), выполняющийся асинхронно прямо сейчас

    QString getPythonExecutable(const CachedConfig &cfg) const;

    bool m_isWaitingForPip = false;
    CachedConfig m_delayedCfg;
    QVariantMap m_delayedParams;

    std::function<QVariantMap(int, const QString&, const QVariantMap&)> m_bridgeHandler;
};

#endif // PLUGINENGINE_H