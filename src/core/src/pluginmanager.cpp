#include "finwizard/pluginmanager.h"
#include <QDebug>

PluginManager::PluginManager(QObject *parent)
    : QObject(parent)
    , m_settings("FinWizard", "PluginManager")
    , m_repository(m_settings) // Передаем настройки Складу
{
    // При старте программы автоматически сканируем кэш
    m_repository.refreshPlugins();

    // Безопасные прокси-коннекты от движка к менеджеру
    connect(&m_engine, &PluginEngine::pipLogReady, this, &PluginManager::pluginLogReceived, Qt::UniqueConnection);
    connect(&m_engine, &PluginEngine::pipFinished, this, &PluginManager::pluginReadyChanged, Qt::UniqueConnection);
    connect(&m_engine, &PluginEngine::pluginFinished, this, &PluginManager::pluginFinished, Qt::UniqueConnection);
    connect(&m_engine, &PluginEngine::infoLogRequested, this, &PluginManager::infoLogRequested);
    connect(&m_engine, &PluginEngine::pluginProgress, this, &PluginManager::pluginProgress, Qt::UniqueConnection);
    connect(&m_engine, &PluginEngine::pluginLogRequested, this, &PluginManager::pluginLiveLogReceived, Qt::UniqueConnection);
}

PluginManager::~PluginManager()
{
    // ЖЕСТКИЙ ФИКС УТЕЧЕК И UB:
    // Явно разрываем связи движка с менеджером перед уничтожением полей,
    // чтобы доживающие свой век в deleteLater процессы pip не слали сигналы в мертвый класс.
    m_engine.disconnect(this);
}

QPair<int, QString> PluginManager::addConfigFromArchive(const QString &archivePath)
{
    // 1. Проверяем, загружен ли уже плагин с таким путем архива.
    // Если да — выгружаем его из движка и освобождаем его зависимости общего
    // пула, ДО того как репозиторий ниже перезапишет/удалит его cachePath
    // (для этого же archivePath repository сам удаляет старую папку кэша —
    // тот путь не проходит через PluginManager::removeConfig, так что
    // освобождение зависимостей нужно продублировать здесь).
    for (int id : m_repository.getAllConfigIds()) {
        const CachedConfig* cfg = m_repository.getConfig(id);
        if (cfg && cfg->originalZipPath == archivePath) {
            m_engine.releasePluginDependencies(id, cfg->cachePath);
            m_engine.unloadPlugin(id); // Выгружаем динамическую либу/скрипт
        }
    }

    // 2. Делегируем добавление репозиторию
    return m_repository.addConfigFromArchive(archivePath);
}

void PluginManager::removeConfig(int id)
{
    // Освобождаем зависимости общего пула ДО удаления папки плагина —
    // releasePluginDependencies() читает shared_deps.json из cachePath,
    // после removeConfig() репозитория читать будет уже нечего.
    const CachedConfig* cfg = m_repository.getConfig(id);
    if (cfg) {
        m_engine.releasePluginDependencies(id, cfg->cachePath);
    }

    // Перед удалением файлов с диска обязательно выгружаем плагин из памяти движка
    m_engine.unloadPlugin(id);
    m_repository.removeConfig(id);
}

QVariantMap PluginManager::runConfig(int id, const QVariantMap &params)
{
    const CachedConfig* cfg = m_repository.getConfig(id);
    if (!cfg) {
        qWarning() << "[МЕНЕДЖЕР] Попытка запустить несуществующий плагин с ID:" << id;
        QVariantMap result;
        result["success"] = false;
        result["error"] = "Плагин не найден в репозитории.";
        return result;
    }

    if (!cfg->isValid) {
        qWarning() << "[МЕНЕДЖЕР] Попытка запустить невалидный плагин с ID:" << id;
        QVariantMap result;
        result["success"] = false;
        result["error"] = "Конфигурация плагина невалидна: " + cfg->validationMessage;
        return result;
    }

    qInfo() << "[МЕНЕДЖЕР] Передаю плагин" << cfg->displayName << "(ID:" << id << ") в движок...";

    // Движок сам разберется: если зависимости стоят — выполнит синхронно и вернет результат;
    // если нет — асинхронно запустит PIP (вернет isInitializing=true), а по завершении установки
    // сам выполнит отложенный запуск и пришлет результат через сигнал pluginFinished.
    return m_engine.runPlugin(*cfg, params);
}

const CachedConfig* PluginManager::getConfig(int id) const
{
    return m_repository.getConfig(id);
}

void PluginManager::unloadPlugin(int id)
{
    m_engine.unloadPlugin(id);
}

QList<int> PluginManager::getAllConfigIds() const
{
    return m_repository.getAllConfigIds();
}

QStringList PluginManager::getDisplayNames() const
{
    QStringList names;

    for (int id : m_repository.getAllConfigIds()) {
        const CachedConfig* cfg = m_repository.getConfig(id);
        if (cfg) {
            // Формируем красивое имя для списков
            QString name = cfg->displayName;
            name += QString(" (ID %1)").arg(id);
            names << name;
        }
    }

    // Сортируем по алфавиту
    names.sort(Qt::CaseInsensitive);
    return names;
}

QMap<int, QString> PluginManager::getAvailableConfigs() const
{
    QMap<int, QString> configs;

    for (int id : m_repository.getAllConfigIds()) {
        const CachedConfig* cfg = m_repository.getConfig(id);
        if (cfg) {
            // Формируем красивое имя для списков
            QString name = cfg->displayName;
            name += QString(" (ID %1)").arg(id);
            configs.insert(id, name);
        }
    }

    return configs;
}

QMap<QString, QString> PluginManager::getConfigPreview(int id) const
{
    return m_repository.getConfigPreview(id);
}

QString PluginManager::getCacheBasePath() const
{
    return m_repository.getCacheBasePath();
}

void PluginManager::setCacheBasePath(const QString &path)
{
    // Защита: не меняем пути на лету, если ставится плагин или сам плагин сейчас выполняется —
    // иначе можно выдернуть кэш/входные файлы прямо из-под работающего процесса.
    if (m_engine.isWaitingForPip()) {
        qWarning() << "[МЕНЕДЖЕР] Смена пути кэша заблокирована: работает PIP!";
        return;
    }
    if (m_engine.isRunningExternalProcess()) {
        qWarning() << "[МЕНЕДЖЕР] Смена пути кэша заблокирована: выполняется плагин!";
        return;
    }

    // При смене папки нужно выгрузить все текущие плагины из памяти
    for (int id : m_repository.getAllConfigIds()) {
        m_engine.unloadPlugin(id);
    }

    m_repository.setCacheBasePath(path);
}

void PluginManager::refreshPlugins()
{
    if (m_engine.isWaitingForPip()) {
        qWarning() << "[МЕНЕДЖЕР] Фоновое сканирование отклонено: в данный момент ставится PIP зависимость.";
        return;
    }

    m_repository.refreshPlugins();
}