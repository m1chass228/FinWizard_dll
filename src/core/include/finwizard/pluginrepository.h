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

    QPair<int, QString> addConfigFromFile(const QString &filePath);
    void refreshPlugins();
    void removeConfig(int id);

    const CachedConfig* getConfig(int id) const;
    QList<int> getAllConfigIds() const;
    QMap<QString, QString> getConfigPreview(int id) const;

    QString getCacheBasePath() const;
    void setCacheBasePath(const QString &path);

private:
    int nextAvailableId() const;
    QString createCacheDirForId(int id);
    bool extractArchive(const QString &archivePath, const QString &targetDir);
    CachedConfig parseManifest(const QString &dirPath) const;

    std::map<int, CachedConfig> m_configs;
    QSettings &m_settings;

    std::vector<char> decompressGzip(const std::string& gzFilePath);

    bool extractTarFromBuffer(const std::vector<char>& tarBuffer, const QString& destDir);

    bool extractPureTar(const QString& tarFilePath, const QString& destDir);
};

#endif // PLUGINREPOSITORY_H
