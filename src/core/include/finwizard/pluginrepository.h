#ifndef PLUGINREPOSITORY_H
#define PLUGINREPOSITORY_H

#include <QString>
#include <QStringList>
#include <QMap>
#include <QSettings>
#include <QDateTime>
#include <map>

struct CachedConfig {
    int id = 0;
    bool isValid = false;
    QString validationMessage;
    QString displayName;
    QString description;
    QString configType;
    QString entryPoint;
    QString originalZipPath;
    QString cachePath;
    QDateTime lastExtracted;
    QDateTime lastUsed;
};

class PluginRepository
{
public:
    explicit PluginRepository(QSettings &settings);

    QPair<int, QString> addConfigFromArchive(const QString &filePath);
    void refreshPlugins();
    void removeConfig(int id);

    const CachedConfig* getConfig(int id) const;
    QList<int> getAllConfigIds() const;
    QMap<QString, QString> getConfigPreview(int id) const;

    QString getCacheBasePath() const;
    void setCacheBasePath(const QString &path);

private:
    int nextAvailableId();
    QString createCacheDirForId(int id);
    CachedConfig parseManifest(const QString &dirPath) const;

    std::map<int, CachedConfig> m_configs;
    QSettings &m_settings;
};

#endif // PLUGINREPOSITORY_H