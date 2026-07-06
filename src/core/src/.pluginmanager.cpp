#include <finwizard/pluginmanager.h>
#include <finwizard/iconfig.h>

// все include, которые нужны только для реализации
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonParseError>
#include <QPluginLoader>
#include <QDebug>
#include <QCoreApplication>
#include <QSettings>

#include <quazip/quazip.h>
#include <quazip/quazipfile.h>

// фрагмент конструктора и деструктора
PluginManager::PluginManager(QObject *parent)
    : QObject(parent)
    , m_settings("FinWizard", "PluginManager")
{
    // Можно здесь загрузить последний id из настроек
    m_nextConfigId = m_settings.value("lastConfigId", 1).toInt();
}

PluginManager::~PluginManager()
{
    // 1. Сначала очищаем список "активных" указателей
    m_activeConfigs.clear();

    // 2. Затем выгружаем плагины
    for (auto it = m_loaders.begin(); it != m_loaders.end(); ++it) {
        if (it->second && it->second->isLoaded()) {
            it->second->unload();
        }
    }
    m_loaders.clear();

    // Сохраняем последний id
    m_settings.setValue("lastConfigId", m_nextConfigId);
    m_settings.sync();
}

bool PluginManager::runConfig(int id, const QVariantMap &params)
{
    // Проверяем, загружен ли уже
    IConfig *plugin = getLoadedConfig(id);
    if (!plugin) {
        // Автоматическая загрузка, если не загружен
        if (!loadPlugin(id)) {
            qWarning() << "Не удалось загрузить конфиг" << id;
            return false;
        }
        plugin = getLoadedConfig(id);
        if (!plugin) {
            qWarning() << "Плагин после загрузки недоступен" << id;
            return false;
        }
    }

    QVariantMap result = plugin->execute(params);

    // Можно логировать результат
    if (result.value("success").toBool()) {
        qInfo() << "Конфиг" << id << "выполнен успешно:" << result.value("message").toString();
    } else {
        qWarning() << "Ошибка выполнения конфига" << id << ":" << result.value("error").toString();
    }

    return result.value("success").toBool();
}

int PluginManager::addConfigFromZip(const QString &zipath)
{
    if (!QFile::exists(zipath)) return -1;

    QFileInfo fi(zipath);
    QString zipBase = fi.baseName();

    // Проверяем, есть ли уже такой zip
    int existingId = -1;
    for (const auto &[id, cfg] : m_configs) {
        if (cfg.originalZipPath == zipath) {
            existingId = id;
            break;
        }
    }

    // Если есть и zip не новее — возвращаем старый
    if (existingId != -1) {
        const auto &old = m_configs[existingId];
        if (fi.lastModified() <= old.lastExtracted) {
            m_configs[existingId].lastUsed = QDateTime::currentDateTime();
            return existingId;
        }
        // Иначе удаляем старое
        QDir(old.cachePath).removeRecursively();
        unloadPlugin(existingId);  // если был загружен
        m_configs.erase(existingId);
    }

    int id = nextAvailableId();

    QString cacheDir = createCacheDirForId(id);
    if (cacheDir.isEmpty()) return -1;

    if (!extractZip(zipath, cacheDir)) {
        QDir(cacheDir).removeRecursively();
        return -1;
    }

    CachedConfig cfg = readMetadata(cacheDir, zipath, id);
    if (!cfg.isValid) {
        QDir(cacheDir).removeRecursively();
        return -1;
    }

    // Подготавливаем зависимости (addLibraryPath, PATH и т.д.)
    if (!prepareDependencies(cacheDir)) {
        cfg.validationMessage = "Не удалось подготовить зависимости";
        cfg.isValid = false;
    }

    cfg.id = id;
    cfg.lastExtracted = QDateTime::currentDateTime();
    cfg.lastUsed = cfg.lastExtracted;

    m_configs[id] = std::move(cfg);

    return id;
}

// -------- Load/Unload Plugins -------
bool PluginManager::loadPlugin(int id)
{
    // 1. Проверка конфига
    auto itCfg = m_configs.find(id);
    if (itCfg == m_configs.end() || !itCfg->second.isValid) return false;

    // 2. Если уже загружен — выходим
    if (m_loaders.contains(id)) return true;

    CachedConfig &cfg = itCfg->second;
    QString pluginPath = cfg.cachePath + QDir::separator() + cfg.entryPoint;

    // 3. Создаем лоадер
    auto loader = std::make_unique<QPluginLoader>(pluginPath);

    // Подготовка окружения (пути к либам)
    prepareDependencies(cfg.cachePath);

    if (!loader->load()) {
        cfg.validationMessage = "Ошибка загрузки: " + loader->errorString();
        qWarning() << cfg.validationMessage;
        return false;
    }

    // 4. Получаем объект
    QObject *obj = loader->instance();
    if (!obj) {
        qWarning() << "Ошибка: loader не смог создать экземпляр объекта!";
        return false;
    }

    IConfig *config = qobject_cast<IConfig*>(obj);
    if (!config) {
        qWarning() << "Критическая ошибка: Плагин загружен, но интерфейс IConfig не распознан. Проверьте IID!";
        return false; // Теперь программа не упадет, а просто напишет ошибку
    }



    // 5. Сохраняем и лоадер, и указатель на интерфейс
    m_activeConfigs[id] = config;
    m_loaders[id] = std::move(loader);

    qInfo() << "Плагин успешно загружен:" << cfg.displayName;
    return true;
}


// bool
void PluginManager::unloadPlugin(int id)
{
    // Сначала удаляем интерфейс из списка активных, чтобы никто не вызвал его случайно
    m_activeConfigs.erase(id);

    auto it = m_loaders.find(id);
    if (it == m_loaders.end()) return;

    if (it->second && it->second->isLoaded()) {
        it->second->unload();
        qDebug() << "Плагин" << id << "выгружен";
        m_loaders.erase(it);
    }

    m_activeConfigs.erase(id);  // ← чистим указатель на объекты
    return;
}

// helpers
const CachedConfig* PluginManager::getConfig(int id) const
{
    auto it = m_configs.find(id);
    if (it != m_configs.end()) {
        return &it->second;
    }
    return nullptr;
}

IConfig* PluginManager::getLoadedConfig(int id) const
{
    auto it = m_activeConfigs.find(id);
    return (it != m_activeConfigs.end()) ? it->second : nullptr;
}

QStringList PluginManager::getDisplayNames() const
{
    QStringList names;
    for (const auto &[id, cfg] : m_configs) {
        // Берём displayName из CachedConfig (который заполняется из manifest.json)
        // Если displayName пустой — fallback на имя ZIP-файла или что-то осмысленное
        QString name = cfg.displayName;
        if (name.isEmpty()) {
            QFileInfo fi(cfg.originalZipPath);
            name = fi.baseName();  // имя файла без расширения
        }
        // Можно добавить ID или статус для наглядности (опционально)
        name += QString(" (ID %1)").arg(id);

        names << name;
    }
    // Сортируем по алфавиту, чтобы было удобно выбирать
    names.sort(Qt::CaseInsensitive);
    return names;
}

CachedConfig PluginManager::loadConfigFromDir(const QString &dirPath)
{
    CachedConfig cfg;
    cfg.isValid = false;
    cfg.cachePath = dirPath;

    QFile file(dirPath + "/manifest.json");
    if (!file.open(QIODevice::ReadOnly)) return cfg;

    QJsonObject obj = QJsonDocument::fromJson(file.readAll()).object();

    // 1. Имя плагина
    cfg.displayName = obj.value("name").toString();

    // 1. комментарий
    cfg.description = obj.value("description").toString();

    // 2. ИМЯ ФАЙЛА DLL (проверь, чтобы в manifest.json ключ был "main_dll")
    QString dllName = obj.value("entry").toString();

    // 3. ФОРМИРУЕМ ПОЛНЫЙ ПУТЬ
    if (!dllName.isEmpty()) {
        // Важно: absoluteFilePath соединит путь к папке и имя файла
        cfg.entryPoint = QDir(dirPath).absoluteFilePath(dllName);
    }

    // ЛОГ ДЛЯ ПРОВЕРКИ
    qDebug() << "--- Проверка внутри loadConfigFromDir ---";
    qDebug() << "Folder:" << dirPath;
    qDebug() << "DLL Name from JSON:" << dllName; // Если тут пусто, значит в JSON нет ключа main_dll
    qDebug() << "Full EntryPoint:" << cfg.entryPoint;

    // 4. ВАЛИДАЦИЯ
    if (!cfg.displayName.isEmpty() && !dllName.isEmpty() && QFile::exists(cfg.entryPoint)) {
        cfg.isValid = true;
    }

    return cfg;
}
void PluginManager::refreshPlugins()
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
        CachedConfig cfg = loadConfigFromDir(fullPath);
        if (cfg.isValid) {
            cfg.id = id;
            m_configs[id] = cfg;
        }
    }

    qDebug() << "Найдено валидных конфигов:" << m_configs.size();
}

QList<int> PluginManager::getAllConfigIds() const
{
    QList<int> ids;
    for (const auto &[id, cfg] : m_configs) {  // ← const auto& — обязательно
        ids << id;
    }
    return ids;
}

QString PluginManager::getCacheBasePath() const
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

void PluginManager::setCacheBasePath(const QString &path)
{
    refreshPlugins();
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
}

QString PluginManager::createCacheDirForId(int id)
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

int PluginManager::nextAvailableId()
{
    if (m_configs.empty()) {
        return 1; // Если мапа пуста, начинаем с 1
    }

    auto itMax = m_configs.begin();
    for (auto it = m_configs.begin(); it != m_configs.end(); it++) {
        if (it->first > itMax->first) {
            itMax = it;
        }
    }

    return itMax->first + 1;
}

bool PluginManager::extractZip(const QString &zipPath, const QString &targetDir)
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

CachedConfig PluginManager::readMetadata(const QString &extractedDir, const QString &zipPath, int assignedId)
{
    CachedConfig cfg;
    cfg.id = assignedId;
    cfg.originalZipPath = zipPath;
    cfg.cachePath = extractedDir;

    // Путь к manifest.json
    QString manifestPath = extractedDir + QDir::separator() + "manifest.json";

    QFile file(manifestPath);
    if (!file.exists()) {
        cfg.validationMessage = "Файл manifest.json не найден";
        return cfg;
    }

    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            cfg.validationMessage = "Не удалось открыть manifest.json: " + file.errorString();
            return cfg;
        }

    QByteArray data = file.readAll();
    file.close();

    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);

    if (parseError.error != QJsonParseError::NoError) {
        qDebug() << "Ошибка парсинга JSON:" << parseError.errorString();
        qDebug() << "Смещение ошибки (символ):" << parseError.offset;
        return cfg; // Или другая обработка ошибки
    }

    if (doc.isNull()) {
        cfg.validationMessage = QString("Ошибка парсинга JSON: %1 (offset %2)")
            .arg(parseError.errorString())
            .arg(parseError.offset);
        return cfg;
    }

    if (!doc.isObject()) {
            cfg.validationMessage = "Корень manifest.json не является объектом";
            return cfg;
        }
    // в объект (словарь), с которым удобно работать через ключи
    QJsonObject obj = doc.object();

    // Обязательные / важные поля
    cfg.displayName = obj.value("name").toString().trimmed();
    if (cfg.displayName.isEmpty()) {
        // Fallback — имя zip-файла без расширения
        QFileInfo zipFi(zipPath);
        cfg.displayName = zipFi.baseName();
    }

    cfg.description = obj.value("description").toString().trimmed();

    cfg.entryPoint = obj.value("entry").toString().trimmed();
    if (cfg.entryPoint.isEmpty()) {
        cfg.validationMessage = "Не указано поле 'entry' (имя плагина)";
        return cfg;
    }

    cfg.configType = obj.value("type").toString("cpp-plugin").trimmed();

    // Опциональные поля (можно расширять)
    if (QJsonArray req = obj.value("requires").toArray(); !req.isEmpty()) {
        for (const QJsonValue &v : req) {
            QString pkg = v.toString().trimmed();
            if (!pkg.isEmpty()) cfg.requiredPythonPackages << pkg;
        }
    }

    // Проверяем минимальную валидность
    cfg.isValid = !cfg.entryPoint.isEmpty();
    if (cfg.isValid) {
        cfg.validationMessage = "OK";
    } else {
        cfg.validationMessage = "Плагин невалиден: отсутствует entryPoint";
    }

    return cfg;
}

bool PluginManager::prepareDependencies(const QString &cacheDir)
{
    // 1. Добавляем папку кэша в пути поиска библиотек Qt
    // Это помогает Qt находить плагины и их зависимости
    QCoreApplication::addLibraryPath(cacheDir);

    // 2. На Windows чаще всего этого недостаточно → модифицируем PATH
    #ifdef Q_OS_WIN
        QByteArray oldPath = qgetenv("PATH");
        QByteArray newPath = oldPath + ";" + cacheDir.toUtf8();
        if (!qputenv("PATH", newPath)) {
            qWarning() << "Не удалось изменить PATH";
            return false;
        }
        qDebug() << "PATH расширен для зависимостей:" << cacheDir;
    #endif

    // 3. На Linux — аналогично LD_LIBRARY_PATH (часто не обязательно, но полезно)
    #ifdef Q_OS_LINUX
        QByteArray oldLd = qgetenv("LD_LIBRARY_PATH");
        QByteArray newLd = oldLd + ":" + cacheDir.toUtf8();
        if (!qputenv("LD_LIBRARY_PATH", newLd)) {
            qWarning() << "Не удалось изменить LD_LIBRARY_PATH";
            return false;
        }
        qDebug() << "LD_LIBRARY_PATH расширен:" << cacheDir;
    #endif

    // 4. Опционально: проверяем наличие самого плагина
    // (entryPoint уже известен из manifest, но можно проверить)
    // QString pluginPath = cacheDir + "/" + cfg.entryPoint;
    // if (!QFile::exists(pluginPath)) {
    //     qWarning() << "Файл плагина не найден:" << pluginPath;
    //     return false;
    // }

    return true;
}

bool PluginManager::attemptLoadPlugin(int id)
{
    auto cfgIt = m_configs.find(id);
    if (cfgIt == m_configs.end() || !cfgIt->second.isValid) {
        qWarning() << "Конфиг" << id << "не найден или невалиден";
        return false;
    }

    CachedConfig &cfg = cfgIt->second;

    // Проверяем, не загружен ли уже
    if (m_loaders.contains(id)) {
        qDebug() << "Плагин" << id << "уже загружен";
        return true;
    }

    QString pluginPath = cfg.cachePath + QDir::separator() + cfg.entryPoint;

    if (!QFile::exists(pluginPath)) {
            cfg.validationMessage = "Файл плагина не найден: " + cfg.entryPoint;
            cfg.isValid = false;
            return false;
    }

    // Подготавливаем пути (если ещё не сделали)
    if (!prepareDependencies(cfg.cachePath)) {
        cfg.validationMessage = "Не удалось подготовить пути для зависимостей";
        cfg.isValid = false;
        return false;
    }

    QPluginLoader loader(pluginPath);

    if (!loader.load()) {
            cfg.validationMessage = "Ошибка загрузки плагина: " + loader.errorString();
            cfg.isValid = false;
            qWarning() << cfg.validationMessage;
            return false;
    }

    QObject *obj = loader.instance();
    if (!obj) {
        loader.unload();
        cfg.validationMessage = "instance() вернул nullptr";
        cfg.isValid = false;
        return false;
    }

    IConfig *config = qobject_cast<IConfig*>(obj);
    if (!config) {
        loader.unload();
        cfg.validationMessage = "Объект не реализует интерфейс IConfig";
        cfg.isValid = false;
        return false;
    }

    // Успех
    m_loaders.emplace(id, std::make_unique<QPluginLoader>(pluginPath));
    cfg.lastUsed = QDateTime::currentDateTime();
    qInfo() << "Плагин" << cfg.displayName << "(id" << id << ") успешно загружен";

    return true;
}

void PluginManager::loadAllCachedConfigs()
{
    QString baseCache = getCacheBasePath();
    QDir baseDir(baseCache);

    if (!baseDir.exists()) {
        qWarning() << "Папка кэша не найдена:" << baseCache;
        return;
    }

    // Ищем все подпапки с числовыми именами (ID)
    QStringList idDirs = baseDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &idStr : idDirs) {
        bool ok;
        int id = idStr.toInt(&ok);
        if (!ok || id <= 0) continue;

        QString cacheDir = baseDir.absoluteFilePath(idStr);
        QString manifestPath = cacheDir + "/manifest.json";

        if (QFile::exists(manifestPath)) {
            // Пытаемся прочитать метаданные (как в addConfigFromZip, но без ZIP)
            QFile file(manifestPath);
            if (file.open(QIODevice::ReadOnly)) {
                QByteArray data = file.readAll();
                QJsonDocument doc = QJsonDocument::fromJson(data);
                if (doc.isObject()) {
                    QJsonObject obj = doc.object();

                    CachedConfig cfg;
                    cfg.id = id;
                    cfg.cachePath = cacheDir;
                    cfg.entryPoint = obj.value("entry").toString();
                    cfg.displayName = obj.value("name").toString();
                    cfg.description = obj.value("description").toString();
                    cfg.configType = obj.value("type").toString("cpp-plugin");
                    cfg.isValid = !cfg.entryPoint.isEmpty();
                    cfg.validationMessage = cfg.isValid ? "OK" : "Нет entryPoint";

                    // Путь к оригинальному ZIP — если есть файл original_zip.txt или что-то
                    // или просто оставляем пустым
                    cfg.originalZipPath = "";  // можно потом доработать

                    cfg.lastExtracted = QFileInfo(cacheDir).lastModified();
                    cfg.lastUsed = cfg.lastExtracted;

                    m_configs[id] = cfg;
                    qDebug() << "Загружен кэшированный конфиг ID" << id << "из" << cacheDir;
                }
            }
        }
    }

    qInfo() << "Загружено кэшированных конфигов:" << m_configs.size();
}

// В заголовке (h) не забудь: QMap<QString, QString> getConfigPreview(int id) const;

QMap<QString, QString> PluginManager::getConfigPreview(int id) const
{
    QMap<QString, QString> preview; // Исправил опечатку в prewiew

    // 1. Ищем конфиг в памяти (быстро)
    auto it = m_configs.find(id);
    if (it == m_configs.end() || !it->second.isValid) {
        return preview; // Возвращаем пустую карту
    }

    const CachedConfig &cfg = it->second;

    // 2. Заполняем данными из уже загруженного конфига (НЕ читаем диск лишний раз!)
    preview["name"] = cfg.displayName;
    preview["description"] = cfg.description;
    preview["type"] = cfg.configType;

    // 3. А вот иконку ищем на диске (так как путь к ней мы не хранили)
    QStringList iconNames = {
        "preview.png", "icon.png", "thumbnail.png", "cover.png",
        "preview.jpg", "icon.jpg", "preview.jpeg"
    };

    QDir dir(cfg.cachePath);
    for (const QString &name : iconNames) {
        QString fullPath = dir.absoluteFilePath(name);
        if (QFile::exists(fullPath)) {
            preview["icon"] = fullPath;
            break; // Нашли первую попавшуюся — хватит
        }
    }

    return preview;
}





