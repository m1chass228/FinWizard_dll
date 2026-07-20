#include "finwizard/pluginrepository.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDebug>
#include <QSet>
#include <QCryptographicHash>

#include <quazip/quazip.h>
#include <quazip/quazipfile.h>

#include "finwizard/archivemanager.h"
#include "finwizard/version.h"

namespace {
// Сравнивает две версии вида "MAJOR.MINOR.PATCH" (частей может быть меньше —
// недостающие считаются нулями). Возвращает <0, если a < b; 0, если равны;
// >0, если a > b. Нечисловые/пустые компоненты трактуются как 0, чтобы
// кривая строка в манифесте не роняла парсинг, а просто вела себя как "0".
int compareVersions(const QString &a, const QString &b)
{
    QStringList partsA = a.split('.');
    QStringList partsB = b.split('.');
    int count = qMax(partsA.size(), partsB.size());

    for (int i = 0; i < count; ++i) {
        int valA = (i < partsA.size()) ? partsA[i].toInt() : 0;
        int valB = (i < partsB.size()) ? partsB[i].toInt() : 0;
        if (valA != valB) return valA - valB;
    }
    return 0;
}
} // namespace

PluginRepository::PluginRepository(QSettings &settings) : m_settings(settings) {}

// ----------------- AVAILABLE ID ------------------------------

int PluginRepository::nextAvailableId() {
    QString base = getCacheBasePath();
    QDir cacheDir(base);

    if (!cacheDir.exists()) {
        cacheDir.mkpath(".");
    }

    // Ищем самый маленький свободный ID начиная с 1
    for (int id = 1; id < 10000; ++id) {  // разумный лимит
        QString dirName = QString::number(id);
        if (!QDir(base + "/" + dirName).exists()) {
            m_settings.setValue("plugins/last_used_id", id);
            return id;
        }
    }

    // Если вдруг всё забито (маловероятно)
    int lastId = m_settings.value("plugins/last_used_id", 0).toInt();
    int nextId = qMax(lastId + 1, 1);
    m_settings.setValue("plugins/last_used_id", nextId);
    return nextId;
}

// ----------------- GET CONFIGS --------------------------------

const CachedConfig* PluginRepository::getConfig(int id) const {
    auto it = m_configs.find(id);
    return (it != m_configs.end()) ? &it->second : nullptr;
}

QList<int> PluginRepository::getAllConfigIds() const {
    QList<int> ids;
    for (const auto &[id, cfg] : m_configs) ids << id;
    return ids;
}



// ----------------- ADD CONFIG (ZIP)----------------------------

QPair<int, QString> PluginRepository::addConfigFromArchive(const QString &filePath)
{
    if (!QFile::exists(filePath)) {
        return {-1, "Файл не существует."};
    }

    QFileInfo fi(filePath);

    // 1. Хэш ДО распаковки
    QByteArray fileHash;
    {
        QFile f(filePath);
        if (f.open(QIODevice::ReadOnly)) {
            QCryptographicHash hash(QCryptographicHash::Md5);
            char buffer[64*1024];
            while (!f.atEnd()) {
                qint64 bytes = f.read(buffer, sizeof(buffer));
                if (bytes > 0) hash.addData(QByteArrayView(buffer, bytes));
            }
            fileHash = hash.result();
        }
    }

    qDebug() << "Хэш:" << fileHash.toHex().left(16);

    // 2. Проверка дубликата
    for (const auto &[exId, exCfg] : m_configs) {
        if (exCfg.archiveHash == fileHash) {
            m_configs[exId].lastUsed = QDateTime::currentDateTime();
            qDebug() << "Дубликат найден по хэшу, ID:" << exId;
            return {exId, "Плагин уже добавлен ранее."};
        }
    }

    // 3. Распаковка
    int id = nextAvailableId();
    QString cacheDir = createCacheDirForId(id);
    if (cacheDir.isEmpty()) return {-1, "Не удалось создать папку."};

    if (!ArchiveManager::extractArchive(filePath, cacheDir)) {
        QDir(cacheDir).removeRecursively();
        return {-1, "Ошибка распаковки."};
    }

    // 4. Парсинг
    CachedConfig cfg = parseManifest(cacheDir);
    if (!cfg.isValid) {
        QDir(cacheDir).removeRecursively();
        return {-1, "Ошибка манифеста."};
    }

    // 5. Имя с суффиксом
    QString baseName = cfg.displayName.trimmed();
    if (baseName.isEmpty()) baseName = fi.baseName();

    QString candidate = baseName;
    int suffix = 1;
    while (true) {
        bool dup = false;
        for (const auto &[exId, exCfg] : m_configs) {
            if (exCfg.displayName.compare(candidate, Qt::CaseInsensitive) == 0) {
                dup = true;
                break;
            }
        }
        if (!dup) break;
        candidate = baseName + " (" + QString::number(suffix++) + ")";
    }
    cfg.displayName = candidate;

    // 6. Сохраняем хэш
    cfg.archiveHash = fileHash;
    cfg.id = id;
    cfg.originalZipPath = filePath;  // на всякий
    cfg.lastExtracted = QDateTime::currentDateTime();
    cfg.lastUsed = cfg.lastExtracted;

    // 7. Записываем хэш в manifest.json
    QFile manifestFile(cacheDir + "/manifest.json");
    if (manifestFile.open(QIODevice::ReadWrite)) {
        QJsonDocument doc = QJsonDocument::fromJson(manifestFile.readAll());
        QJsonObject obj = doc.object();
        obj["archive_hash"] = QString(fileHash.toHex());
        doc.setObject(obj);
        manifestFile.seek(0);
        manifestFile.write(doc.toJson());
        manifestFile.resize(manifestFile.pos());
        manifestFile.close();
    }

    m_configs[id] = std::move(cfg);

    return {id, ""};
}

// ----------------- REMOVE CONFIG  -------------------------------

void PluginRepository::removeConfig(int id)
{
    auto it = m_configs.find(id);
    if (it != m_configs.end()) {
        QDir(it->second.cachePath).removeRecursively();
        m_configs.erase(it);
    }
}

// -----------------  REFRESH PLUGINS  ----------------------------
void PluginRepository::refreshPlugins()
{
    QString basePath = getCacheBasePath();
    QDir cacheDir(basePath);

    qDebug() << "--- SCANNING CACHE --- Path:" << basePath;

    m_configs.clear();

    if (!cacheDir.exists()) {
        qDebug() << "Cache directory does not exist!";
        return;
    }

    QStringList subDirs = cacheDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);

    for (const QString &dirName : subDirs) {
        bool ok = false;
        int id = dirName.toInt(&ok);
        if (!ok || id <= 0) {
            QDir(cacheDir.absoluteFilePath(dirName)).removeRecursively();
            continue;
        }

        QString fullPath = cacheDir.absoluteFilePath(dirName);

        if (QDir(fullPath).entryList(QDir::Files | QDir::NoDotAndDotDot).isEmpty()) {
            QDir(fullPath).removeRecursively();
            continue;
        }

        CachedConfig cfg = parseManifest(fullPath);
        if (cfg.isValid) {
            cfg.id = id;

            // Восстанавливаем хэш из оригинального архива (если есть)
            if (!cfg.originalZipPath.isEmpty() && QFile::exists(cfg.originalZipPath)) {
                QFile f(cfg.originalZipPath);
                if (f.open(QIODevice::ReadOnly)) {
                    QCryptographicHash hash(QCryptographicHash::Md5);
                    char buffer[64*1024];
                    while (!f.atEnd()) {
                        qint64 bytes = f.read(buffer, sizeof(buffer));
                        if (bytes > 0) hash.addData(QByteArrayView(buffer, bytes));
                    }
                    cfg.archiveHash = hash.result();
                }
            }

            m_configs[id] = cfg;
        } else {
            QDir(fullPath).removeRecursively();
        }
    }

    qDebug() << "Найдено валидных конфигов:" << m_configs.size();
}
// ------------------ CACHE ---------------------

QString PluginRepository::getCacheBasePath() const
{
    // дефолт
    QString defaultPath = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (defaultPath.isEmpty()) {
        defaultPath = QDir::tempPath() + "/FinWizard_cache";
    }
    defaultPath = QDir::cleanPath(defaultPath + "/plugins-cache");

    // Читаем из настроек (если ничего нет → вернёт дефолт)
    QString savedPath = m_settings.value("cache/path", defaultPath).toString();

    // Нормализуем и проверяем
    savedPath = QDir::cleanPath(savedPath);

    // Если путь пустой или недоступный — возвращаем дефолт и сбрасываем настройку
    if (savedPath.isEmpty() || !QDir(savedPath).mkpath(".")) {
        const_cast<QSettings&>(m_settings).remove("cache/path");  // const_cast для mutable  // чистим битую настройку
        return defaultPath;
    }

    return savedPath;

}

void PluginRepository::setCacheBasePath(const QString &path)
{
    if (path.isEmpty()) {
        m_settings.remove("cache/path");
    } else {
        QString cleaned = QDir::cleanPath(path);
        if (QDir(cleaned).mkpath(".")) {
            m_settings.setValue("cache/path", cleaned);
            m_settings.sync();
        } else {
            qWarning() << "Не удалось применить путь кэша:" << cleaned;
        }
    }
    refreshPlugins();
}

QString PluginRepository::createCacheDirForId(int id)
{
    QString base = getCacheBasePath();
    QString dirPath = base + QDir::separator() + QString::number(id);

    QDir dir(dirPath);
    if (!dir.mkpath(".")) {
        qWarning() << "Не удалось создать папку кэша:" << dirPath;
        return {};
    }
    return dirPath;
}

// ----- is name already used -----

bool PluginRepository::isNameAlreadyUsed(const QString &name, int excludeId) const
{
    for (const auto &[id, cfg] : m_configs) {
        if (id != excludeId && cfg.displayName.compare(name, Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
}

// ------------------ PARSE MANIFEST ----------------

CachedConfig PluginRepository::parseManifest(const QString &dirPath) const
{
    CachedConfig cfg;
    cfg.isValid = false;
    cfg.cachePath = dirPath;
    cfg.archiveHash = QByteArray();   // важно

    QFile file(dirPath + "/manifest.json");
    if (!file.open(QIODevice::ReadOnly)) {
        cfg.validationMessage = "Не удалось открыть manifest.json";
        return cfg;
    }

    QJsonParseError parseErr;
    QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseErr);
    if (parseErr.error != QJsonParseError::NoError) {
        cfg.validationMessage = QString("manifest.json содержит некорректный JSON (%1, смещение %2).")
                                    .arg(parseErr.errorString()).arg(parseErr.offset);
        return cfg;
    }

    QJsonObject obj = doc.object();
    cfg.displayName = obj.value("name").toString().trimmed();
    cfg.description = obj.value("description").toString().trimmed();
    cfg.configType = obj.value("type").toString("cpp-plugin").trimmed();

    QString hashStr = obj.value("archive_hash").toString();
    if (!hashStr.isEmpty()) {
        cfg.archiveHash = QByteArray::fromHex(hashStr.toLatin1());
    }

    static const QSet<QString> kKnownTypes = {"python-script", "executable", "cpp-plugin"};
    if (!kKnownTypes.contains(cfg.configType)) {
        cfg.validationMessage = QString("Неизвестный тип плагина: \"%1\".").arg(cfg.configType);
        return cfg;
    }

    cfg.version = obj.value("version").toString("1.0.0").trimmed();
    cfg.minEngineVersion = obj.value("min_engine_version").toString().trimmed();

    if (!cfg.minEngineVersion.isEmpty() &&
        compareVersions(FinWizard::kEngineVersion, cfg.minEngineVersion) < 0) {
        cfg.validationMessage = QString("Требуется версия движка %1 или новее.").arg(cfg.minEngineVersion);
        return cfg;
    }

    // Поиск entryPoint
    QString dllName;

#ifdef Q_OS_WIN
    if (obj.contains("entry_win")) dllName = obj.value("entry_win").toString().trimmed();
#elif defined(Q_OS_LINUX)
    if (obj.contains("entry_linux")) dllName = obj.value("entry_linux").toString().trimmed();
#elif defined(Q_OS_MAC)
    if (obj.contains("entry_mac")) dllName = obj.value("entry_mac").toString().trimmed();
#endif

    if (dllName.isEmpty() && obj.contains("entry")) {
        dllName = obj.value("entry").toString().trimmed();
    }

    if (!dllName.isEmpty()) {
        cfg.entryPoint = QDir(dirPath).absoluteFilePath(dllName);

        QString normalizedBase = QDir::cleanPath(QDir(dirPath).absolutePath());
        QString normalizedEntry = QDir::cleanPath(cfg.entryPoint);
        if (normalizedEntry != normalizedBase && !normalizedEntry.startsWith(normalizedBase + "/")) {
            cfg.validationMessage = "Недопустимый путь в entry (path traversal).";
            return cfg;
        }

        if (QFile::exists(cfg.entryPoint)) {
            if (cfg.configType == "python-script" && !cfg.entryPoint.endsWith(".py", Qt::CaseInsensitive)) {
                cfg.validationMessage = "Тип python-script, но файл не .py";
                return cfg;
            }
            cfg.isValid = true;
            cfg.validationMessage = "OK";
        } else {
            cfg.validationMessage = "Файл не найден: " + dllName;
        }
    } else {
        cfg.validationMessage = "Не указан entry в манифесте";
    }

    return cfg;
}

// ----------------- GET PREVIEW -----------------------

QMap<QString, QString> PluginRepository::getConfigPreview(int id) const
{
    QMap<QString, QString> preview;

    // 1. Ищем конфиг в памяти (быстро)
    auto it = m_configs.find(id);
    if (it == m_configs.end() || !it->second.isValid) {
        return preview;
    }

    const CachedConfig &cfg = it->second;

    // 2. Заполняем данными
    preview["name"] = cfg.displayName;
    preview["description"] = cfg.description;
    preview["type"] = cfg.configType;
    preview["version"] = cfg.version;

    // 3. Ищем иконку на диске по точным именам И по маскам "имя_*"
    QStringList nameFilters = {
        // Точные совпадения
        "preview.png", "icon.png", "thumbnail.png", "cover.png",
        "preview.jpg", "icon.jpg", "preview.jpeg",

        // Маски для "icon_слово", "preview_слово" и т.д.
        "icon_*.png", "icon_*.jpg", "icon_*.jpeg",
        "preview_*.png", "preview_*.jpg", "preview_*.jpeg"
    };

    QDir dir(cfg.cachePath);

    // Получаем список только файлов, подходящих под наши фильтры, игнорируя регистр
    QStringList foundFiles = dir.entryList(nameFilters, QDir::Files | QDir::NoDotAndDotDot);

    if (!foundFiles.isEmpty()) {
        // Берем первый найденный файл (entryList вернет их в алфавитном порядке)
        preview["icon"] = dir.absoluteFilePath(foundFiles.first());
    }

    return preview;
}