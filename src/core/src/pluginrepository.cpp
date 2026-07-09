            #include "finwizard/pluginrepository.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDebug>

#include <quazip/quazip.h>
#include <quazip/quazipfile.h>

PluginRepository::PluginRepository(QSettings &settings) : m_settings(settings) {}

// ----------------- AVAILABLE ID ------------------------------

int PluginRepository::nextAvailableId() const {
    // Читаем последний выданный ID из настроек, по дефолту 0
    int lastId = m_settings.value("plugins/last_used_id", 0).toInt();
    int nextId = lastId + 1;

    // На всякий случай проверяем, вдруг папка с таким ID физически существует на диске
    QString base = getCacheBasePath();
    while (QDir(base + QDir::separator() + QString::number(nextId)).exists()) {
        nextId++;
    }

    // Сохраняем новый "счетчик"
    const_cast<QSettings&>(m_settings).setValue("plugins/last_used_id", nextId);
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

QPair<int, QString> PluginRepository::addConfigFromZip(const QString &zipPath)
{
    if (!QFile::exists(zipPath)) {
        return {-1, "Файл ZIP не существует или недоступен для чтения."};
    }

    QFileInfo fi(zipPath);
    int existingId = -1;
    for (const auto &[id, cfg] : m_configs) {
        if (cfg.originalZipPath == zipPath) {
            existingId = id;
            break;
        }
    }

    if (existingId != -1) {
        const auto &old = m_configs[existingId];
        if (fi.lastModified() <= old.lastExtracted) {
            m_configs[existingId].lastUsed = QDateTime::currentDateTime();
            return {existingId, ""}; // Успех, вернули старый
        }
        QDir(old.cachePath).removeRecursively();
        m_configs.erase(existingId);
    }

    int id = (existingId != -1) ? existingId : nextAvailableId();
    QString cacheDir = createCacheDirForId(id);

    if (cacheDir.isEmpty()) {
        return {-1, "Не удалось создать папку кэша: " + cacheDir};
    }

    if (!extractZip(zipPath, cacheDir)) {
        QDir(cacheDir).removeRecursively();
        return {-1, "Не удалось распаковать ZIP-архив. Возможно, файл поврежден."};
    }

    CachedConfig cfg = parseManifest(cacheDir);
    if (!cfg.isValid) {
        QDir(cacheDir).removeRecursively();
        return {-1, "Ошибка в манифесте плагина:\n" + cfg.validationMessage};
    }

    cfg.id = id;
    cfg.originalZipPath = zipPath;
    cfg.lastExtracted = QDateTime::currentDateTime();
    cfg.lastUsed = cfg.lastExtracted;
    if (cfg.displayName.isEmpty()) cfg.displayName = fi.baseName();

    m_configs[id] = std::move(cfg);
    return {id, ""}; // Успех! Ошибок нет.
}

bool PluginRepository::extractZip(const QString &zipPath, const QString &targetDir)
{
    QuaZip zip(zipPath);
    if (!zip.open(QuaZip::mdUnzip)) {
        qWarning() << "ZIP open error:" << zip.getZipError();
        return false;
    }

    QDir dir(targetDir);
    if (!dir.mkpath(".")) return false;

    for (bool more = zip.goToFirstFile(); more; more = zip.goToNextFile()) {
        QString name = zip.getCurrentFileName();
        if (name.endsWith('/')) continue;

        QString outPath = targetDir + QDir::separator() + name;
        QDir().mkpath(QFileInfo(outPath).absolutePath());

        QuaZipFile zf(&zip);
        if (!zf.open(QIODevice::ReadOnly)) continue;

        QFile out(outPath);
        if (!out.open(QIODevice::WriteOnly)) continue;

        out.write(zf.readAll());
        out.close();
    }
    zip.close();
    return true;
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
    QString defaultPath = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (defaultPath.isEmpty()) {
        defaultPath = QDir::tempPath() + "/FinWizard_cache";
    }
    defaultPath = QDir::cleanPath(defaultPath + "/plugins-cache");

    // Читаем из настроек (если ничего нет → вернёт дефолт)
    QString m_cacheBasePath = m_settings.value("cache/path", defaultPath).toString();

    qDebug() << "--- SCANNING CACHE ---";
    qDebug() << "Path:" << m_cacheBasePath;

    qDebug() << "Обновление списка плагинов из:" << m_cacheBasePath;

    // 1. Очищаем текущий кэш описаний (НЕ лоадеры!)
    // Мы оставляем m_loaders и m_activeConfigs нетронутыми,
    // чтобы работающие плагины не "отвалились".
    m_configs.clear();

    QDir cacheDir(m_cacheBasePath);
    if (!cacheDir.exists()) {
        qDebug() << "CRITICAL: Cache directory does not exist!";
        return;
    }
    // 2. Итерируемся по подпапкам (каждая подпапка - один плагин)
    QStringList subDirs = cacheDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    qDebug() << "Found subdirectories:" << subDirs;

    for (const QString &dirName : subDirs) {
        int id = dirName.toInt(); // если у тебя папки называются по ID
        if (id <= 0) continue;

        QString fullPath = cacheDir.absoluteFilePath(dirName);

        // Вызываем твою логику чтения манифеста
        CachedConfig cfg = parseManifest(fullPath);
        if (cfg.isValid) {
            cfg.id = id;
            m_configs[id] = cfg;
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

// ------------------ PARSE MANIFEST ----------------

CachedConfig PluginRepository::parseManifest(const QString &dirPath) const
{
    CachedConfig cfg;
    cfg.isValid = false;
    cfg.cachePath = dirPath;

    QFile file(dirPath + "/manifest.json");
    if (!file.open(QIODevice::ReadOnly)) {
        cfg.validationMessage = "Не удалось открыть manifest.json";
        return cfg;
    }

    QJsonObject obj = QJsonDocument::fromJson(file.readAll()).object();
    cfg.displayName = obj.value("name").toString().trimmed();
    cfg.description = obj.value("description").toString().trimmed();
    cfg.configType = obj.value("type").toString("cpp-plugin").trimmed();

    // --- УМНЫЙ ПОИСК БИНАРНИКА (КРОССПЛАТФОРМА) ---
    QString dllName;

#ifdef Q_OS_WIN
    if (obj.contains("entry_win")) {
        dllName = obj.value("entry_win").toString().trimmed();
    }
#elif defined(Q_OS_LINUX)
    if (obj.contains("entry_linux")) {
        dllName = obj.value("entry_linux").toString().trimmed();
    }
#elif defined(Q_OS_MAC)
    if (obj.contains("entry_mac")) {
        dllName = obj.value("entry_mac").toString().trimmed();
    }
#endif

    // Fallback: Если ОС-специфичного ключа нет или он пустой, берем универсальный "entry"
    if (dllName.isEmpty() && obj.contains("entry")) {
        dllName = obj.value("entry").toString().trimmed();
    }
    // ----------------------------------------------

    if (!dllName.isEmpty()) {
        cfg.entryPoint = QDir(dirPath).absoluteFilePath(dllName);

        if (QFile::exists(cfg.entryPoint)) {
            // --- СТРОГАЯ ПРОВЕРКА ТИПОВ И РАСШИРЕНИЙ ---

            // 1. Проверка для Python-скриптов
            if (cfg.configType == "python-script" && !cfg.entryPoint.endsWith(".py", Qt::CaseInsensitive)) {
                cfg.validationMessage = "Конфиг заявляет тип 'python-script', но файл точки входа не имеет расширения .py";
                return cfg; // isValid остается false, склад забракует плагин
            }

            // 2. Проверка для исполняемых бинарников (на Windows жестко требуем .exe)
            if (cfg.configType == "executable") {
#ifdef Q_OS_WIN
                if (!cfg.entryPoint.endsWith(".exe", Qt::CaseInsensitive)) {
                    cfg.validationMessage = "Конфиг заявляет тип 'executable', но файл на Windows должен иметь расширение .exe";
                    return cfg;
                }
#endif
            }

            // Если все проверки пройдены — плагин легитимен
            cfg.isValid = true;
            cfg.validationMessage = "OK";
        } else {
            cfg.validationMessage = "Файл плагина не найден: " + dllName;
        }
    } else {
        cfg.validationMessage = "В манифесте не указан ключ 'entry' или специфичный для ОС";
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




