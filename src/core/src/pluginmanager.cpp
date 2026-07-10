#include "finwizard/pluginmanager.h"
#include <QDebug>

PluginManager::PluginManager(QObject *parent)
    : QObject(parent)
    , m_settings("FinWizard", "PluginManager")
    , m_repository(m_settings) // Передаем настройки Складу
{
    // При старте программы автоматически сканируем кэш
    m_repository.refreshPlugins();

    connect(&m_engine, &PluginEngine::pipLogReady, this, &PluginManager::pluginLogReceived, Qt::UniqueConnection);
    connect(&m_engine, &PluginEngine::pipFinished, this, &PluginManager::pluginReadyChanged, Qt::UniqueConnection);

    connect(&m_engine, &PluginEngine::pluginFinished, this, &PluginManager::pluginFinished);
}

PluginManager::~PluginManager()
{
    // Нам не нужно писать код очистки памяти!
    // m_engine сам выгрузит все DLL в своем деструкторе.
    // m_settings сам сохранит данные на диск.
}

QPair<int, QString> PluginManager::addConfigFromArchive(const QString &archivePath)
{
    // 1. Проверяем, загружен ли уже плагин с таким путем архива.
    // Если да — выгружаем его из движка, чтобы освободить дескрипторы файлов перед перезаписью.
    for (int id : m_repository.getAllConfigIds()) {
        const CachedConfig* cfg = m_repository.getConfig(id);
        if (cfg && cfg->originalZipPath == archivePath) {
            m_engine.unloadPlugin(id); // Выгружаем динамическую либу/скрипт из памяти
            break;
        }
    }

    return m_repository.addConfigFromFile(archivePath);
}

// В pluginmanager.cpp
void PluginManager::removeConfig(int id) {
    m_repository.removeConfig(id);
}

QVariantMap PluginManager::runConfig(int id, const QVariantMap &params)
{
    // 1. Спрашиваем у Склада описание плагина
    const CachedConfig* cfg = m_repository.getConfig(id);
    if (!cfg || !cfg->isValid) {
        qWarning() << "Попытка запустить невалидный или несуществующий плагин ID:" << id;
        QVariantMap errorResult;
        errorResult["success"] = false;
        errorResult["error"] = QString("Попытка запустить невалидный или несуществующий плагин ID:").arg(id);
        return errorResult;
    }

    // 2. Отдаем конфиг Движку для выполнения
    return m_engine.runPlugin(*cfg, params);
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
    // При смене папки нужно выгрузить все текущие плагины из памяти
    for (int id : m_repository.getAllConfigIds()) {
        m_engine.unloadPlugin(id);
    }

    m_repository.setCacheBasePath(path);
}

void PluginManager::refreshPlugins()
{
    m_repository.refreshPlugins();
}
