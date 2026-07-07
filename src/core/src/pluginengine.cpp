#include "finwizard/pluginengine.h"
#include <QCoreApplication>
#include <QDebug>
#include <QProcess>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QDir>
#include <QCoreApplication>
#include <QStandardPaths>
#include <QJsonArray>

PluginEngine::PluginEngine(QObject *parent)
    : QObject(parent), m_pipProcess(nullptr)
{
}

PluginEngine::~PluginEngine()
{
    // Если процесс pip еще выполняется в момент уничтожения движка
    if (m_pipProcess) {
        // Отключаем все сигналы, чтобы лямбды не вызвались во время уничтожения объекта
        m_pipProcess->disconnect();

        if (m_pipProcess->state() != QProcess::NotRunning) {
            m_pipProcess->kill(); // Принудительно тушим процесс
            m_pipProcess->waitForFinished(1000); // Даем 1 секунду на очистку в ОС
        }

        delete m_pipProcess; // Чистим память вручную, так как деструктор — это финал
        m_pipProcess = nullptr;
    }
}

// ------------------ PREPARE DEPENCIES ----------------------

bool PluginEngine::prepareDependencies(const QString &cacheDir)
{
    // 1. Добавляем папку кэша в пути поиска библиотек Qt
    QCoreApplication::addLibraryPath(cacheDir);

    // 2. На Windows чаще всего этого недостаточно → модифицируем PATH
#ifdef Q_OS_WIN
    QByteArray oldPath = qgetenv("PATH");
    QByteArray pathToAdd = ";" + cacheDir.toUtf8();

    // Добавляем, только если этого пути там еще нет! (защита от утечки памяти ОС)
    if (!oldPath.contains(pathToAdd)) {
        qputenv("PATH", oldPath + pathToAdd);
        qDebug() << "PATH расширен для зависимостей:" << cacheDir;
    }
#endif

    // 3. На Linux — аналогично LD_LIBRARY_PATH
#ifdef Q_OS_LINUX
    QByteArray oldLd = qgetenv("LD_LIBRARY_PATH");
    QByteArray ldToAdd = ":" + cacheDir.toUtf8();

    if (!oldLd.contains(ldToAdd)) {
        qputenv("LD_LIBRARY_PATH", oldLd + ldToAdd);
        qDebug() << "LD_LIBRARY_PATH расширен:" << cacheDir;
    }
#endif

    return true;
}

bool PluginEngine::setupPythonEnvironment(const CachedConfig &cfg)
{
    QString reqPath = cfg.cachePath + "/requirements.txt";
    if (!QFile::exists(reqPath)) {
        QFile manifestFile(cfg.cachePath + "/manifest.json");
        if (manifestFile.open(QIODevice::ReadOnly)) {
            QJsonObject obj = QJsonDocument::fromJson(manifestFile.readAll()).object();
            manifestFile.close(); // Сразу закрываем чтение

            QJsonArray depsArray = obj.value("dependencies").toArray();
            if (!depsArray.isEmpty()) {
                QFile reqFile(reqPath);
                if (reqFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
                    QTextStream stream(&reqFile);
                    for (const QJsonValue &dep : depsArray) {
                        stream << dep.toString().trimmed() << "\n";
                    }
                    reqFile.close();
                    qInfo() << "Requirements.txt успешно сгенерирован автоматически.";
                }
            } else {
                return true; // Зависимостей нет — выходим
            }
        }
    }

    QString venvPath = cfg.cachePath + "/.venv";
    QString venvPython;

#ifdef Q_OS_WIN
    venvPython = venvPath + "/Scripts/python.exe";
#else
    venvPython = venvPath + "/bin/python";
#endif

    // Если маркер уже стоит и venv на месте — всё готово, выходим сразу
    QString installedMarker = venvPath + "/.requirements_installed";
    if (QFile::exists(venvPython) && QFile::exists(installedMarker)) {
        return true;
    }

    QString basePython = getPythonExecutable(cfg);

    // Проверяем, нашли ли хоть какой-то Python
    if (basePython.isEmpty() || (!basePython.contains("python") && !QFile::exists(basePython))) {
        qWarning() << "Критическая ошибка: Базовый Python интерпретатор не найден!";
        return false;
    }

    // 1. Создаем venv (тут пока оставим синхронно, так как это быстро — 1-2 сек)
    if (!QFile::exists(venvPython)) {
        QProcess createVenv;
        createVenv.start(basePython, QStringList() << "-m" << "venv" << venvPath);
        if (!createVenv.waitForFinished(30000) || createVenv.exitCode() != 0) {
            qWarning() << "Не удалось создать venv:" << createVenv.readAllStandardError();
            return false;
        }
    }

    // 2. АСИНХРОННЫЙ ЗАПУСК PIP ДЛЯ GUI
    // Если прошлый процесс еще живой — не запускаем новый
    if (m_pipProcess && m_pipProcess->state() != QProcess::NotRunning) {
        return false;
    }

    if (!m_pipProcess) {
        m_pipProcess = new QProcess(); // Создаем процесс, если еще нет
    }

    // Настраиваем аргументы для pip
    QStringList pipArgs;
    pipArgs << "-m" << "pip" << "install" << "-r" << reqPath;

    // ВАЖНО: Одиночные чистые коннекты
    QObject::connect(m_pipProcess, &QProcess::readyReadStandardOutput, [this, cfg]() {
        QString output = m_pipProcess->readAllStandardOutput().trimmed();
        qDebug() << "[PIP OUT]:" << output;
        emit pipLogReady(cfg.id, output);
    });

    QObject::connect(m_pipProcess, &QProcess::readyReadStandardError, [this, cfg]() {
        QString errorOutput = m_pipProcess->readAllStandardError().trimmed();
        qWarning() << "[PIP ERR]:" << errorOutput;
        emit pipLogReady(cfg.id, "[ERROR] " + errorOutput);
    });

    // Обработка завершения
    // Обработка завершения в setupPythonEnvironment
    QObject::connect(m_pipProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                     [this, cfg, venvPath, installedMarker](int exitCode, QProcess::ExitStatus exitStatus) {

                         bool success = (exitStatus == QProcess::NormalExit && exitCode == 0);

                         if (success) {
                             qInfo() << "Установка зависимостей успешно завершена!";
                             QFile marker(installedMarker);
                             if (marker.open(QIODevice::WriteOnly)) {
                                 marker.close();
                             }

                             // === АВТОСТАРТ ===
                             if (m_isWaitingForPip && m_delayedCfg.id == cfg.id) {
                                 m_isWaitingForPip = false;
                                 qInfo() << "🚀 [ДВИЖОК] Запускаю отложенный Python-скрипт...";

                                 // Запускаем скрипт
                                 QVariantMap runResult = runExternalProcess(m_delayedCfg, m_delayedParams);
                                 qDebug() << "Результат автоматического запуска скрипта движком:" << runResult;

                                 bool scriptSuccess = runResult.value("success").toBool();
                                 QString msg = runResult.value("message").toString();
                                 QString outPath = runResult.value("outputPath").toString();

                                 if (!scriptSuccess && msg.isEmpty()) {
                                     msg = runResult.value("error").toString();
                                 }

                                 // Оповещаем GUI, что плагин ПОЛНОСТЬЮ отработал
                                 emit pluginFinished(cfg.id, scriptSuccess, msg, outPath);
                             } else {
                                 // Если это была просто фоновая установка без ожидания запуска
                                 emit pipFinished(cfg.id, true);
                             }

                         } else {
                             qWarning() << "Pip завершился с ошибкой. Код:" << exitCode;
                             m_isWaitingForPip = false;
                             emit pipFinished(cfg.id, false);
                         }

                         m_pipProcess->deleteLater();
                         m_pipProcess = nullptr;
                     });

    qInfo() << "Запуск асинхронной установки зависимостей для плагина...";
    m_pipProcess->start(venvPython, pipArgs);

    return m_pipProcess->waitForStarted(); // Возвращаем true, если процесс успешно стартовал
}

// ------------------- LOAD PLUGIN ---------------------------

bool PluginEngine::loadPlugin(const CachedConfig &cfg)
{
    // 1. Проверка конфига (Склад мог прислать битый)
    if (!cfg.isValid) return false;

    // 2. ЗАПУСКАЕМ ПОДГОТОВКУ ОКРУЖЕНИЯ ДЛЯ PYTHON ЗДЕСЬ
    // Метод проверит requirements.txt и сам решит, надо ли ставить пакеты.
    // Если это первый запуск, он асинхронно запустит pip в фоне.
    if (cfg.configType == "python-script") {
        setupPythonEnvironment(cfg);
    }

    // 3. Ни EXE-шники, ни Python-скрипты не нужно грузить в оперативку как DLL!
    if (cfg.configType == "executable" || cfg.configType == "python-script") {
        return true; // Для скриптов этого достаточно, выходим
    }

    // 4. Если C++ DLL уже загружен — выходим (успех)
    if (m_loaders.count(cfg.id)) return true; //[cite: 1]

    // 5. Создаем лоадер для C++ DLL[cite: 1]
    auto loader = std::make_unique<QPluginLoader>(cfg.entryPoint); //[cite: 1]

    // Подготовка окружения для DLL (пути к либам)[cite: 1]
    prepareDependencies(cfg.cachePath); //[cite: 1]

    if (!loader->load()) { //[cite: 1]
        qWarning() << "Ошибка загрузки плагина:" << loader->errorString(); //[cite: 1]
        return false; //[cite: 1]
    }

    // 6. Получаем объект интерфейса C++
    QObject *obj = loader->instance();
    if (!obj) {
        loader->unload();
        qWarning() << "Ошибка: loader не смог создать экземпляр объекта!";
        return false;
    }

    IConfig *config = qobject_cast<IConfig*>(obj);
    if (!config) {
        loader->unload();
        qWarning() << "Критическая ошибка: Плагин загружен, но интерфейс IConfig не распознан. Проверьте IID!";
        return false;
    }

    // 7. Сохраняем в память
    m_activeConfigs[cfg.id] = config;
    m_loaders[cfg.id] = std::move(loader);

    qInfo() << "Плагин успешно загружен:" << cfg.displayName;
    return true;
}

// ------------------ UNLOAD PLUGIN ---------------------------

void PluginEngine::unloadPlugin(int id)
{
    // Сначала удаляем интерфейс из списка активных
    m_activeConfigs.erase(id);

    auto it = m_loaders.find(id);
    if (it == m_loaders.end()) return;

    if (it->second && it->second->isLoaded()) {
        it->second->unload();
        qDebug() << "Плагин" << id << "выгружен";
    }

    // Удаляем лоадер
    m_loaders.erase(it);
}

// ---------------- RUN PLUGIN ---------------------------------

QVariantMap PluginEngine::runPlugin(const CachedConfig &cfg, const QVariantMap &params)
{
    qDebug() << "=== СРАБОТАЛ ОБНОВЛЕННЫЙ RUNPLUGIN! ТИП:" << cfg.configType;

    if (cfg.configType == "executable" || cfg.configType == "python-script") {

        if (cfg.configType == "python-script") {
            QString venvPath = cfg.cachePath + "/.venv";
            QString installedMarker = venvPath + "/.requirements_installed";

#ifdef Q_OS_WIN
            QString venvPython = venvPath + "/Scripts/python.exe";
#else
            QString venvPython = venvPath + "/bin/python";
#endif

            if (!QFile::exists(venvPython) || !QFile::exists(installedMarker)) {
                if (m_pipProcess && m_pipProcess->state() != QProcess::NotRunning) {
                    QVariantMap waitResult;
                    waitResult["success"] = false;
                    waitResult["isInitializing"] = true;
                    waitResult["error"] = "Зависимости плагина еще устанавливаются. Пожалуйста, подождите...";
                    return waitResult;
                }

                // === ВОТ ТУТ ЗАПОМИНАЕМ ПАРАМЕТРЫ ДЛЯ АВТОСТАРТА ===
                m_isWaitingForPip = true;
                m_delayedCfg = cfg;
                m_delayedParams = params;

                qInfo() << "Окружение не готово. Запуск инициализации venv для конфига:" << cfg.id;
                setupPythonEnvironment(cfg);

                QVariantMap initResult;
                initResult["success"] = false;
                initResult["isInitializing"] = true;
                initResult["error"] = "Запущена автоматическая установка зависимостей...";
                return initResult;
            }
        }

        return runExternalProcess(cfg, params);
    }

    if (!loadPlugin(cfg)) {
        QVariantMap errorResult;
        errorResult["success"] = false;
        errorResult["error"] = QString("Не удалось загрузить плагин '%1' в память").arg(cfg.displayName);
        return errorResult;
    }

    IConfig *plugin = m_activeConfigs[cfg.id];
    return plugin->execute(params);
}
// ---------------- RUN EXTERNAL PROCESS -------------------------

QVariantMap PluginEngine::runExternalProcess(const CachedConfig &cfg, const QVariantMap &params)
{
    QVariantMap result;

    QString inputPath = cfg.cachePath + "/input.json";
    QString outputPath = cfg.cachePath + "/output.json";

    // 1. Создаем input.json (ЯВНО СТАВИМ UTF-8 И ТЕКСТОВЫЙ РЕЖИМ)
    QJsonDocument docIn(QJsonObject::fromVariantMap(params));
    QFile inFile(inputPath);
    if (inFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        // Насильно сохраняем в UTF-8 без BOM, чтобы Python 100% прочитал русские пути
        inFile.write(docIn.toJson(QJsonDocument::Compact));
        inFile.close();
    } else {
        result["success"] = false;
        result["error"] = "Не удалось создать файл параметров (input.json)";
        return result;
    }

    QFile::remove(outputPath); // Удаляем старый результат, если был

    // 2. ОПРЕДЕЛЯЕМ КОМАНДУ ДЛЯ ЗАПУСКА
    QString program;
    QStringList args;

    if (cfg.configType == "executable") {
        // Просто запускаем бинарник
        program = cfg.entryPoint;
        args << inputPath << outputPath;
    }
    else if (cfg.configType == "python-script") {
        // Используем наш новый хелпер — он сам выберет venv или базу
        program = getPythonExecutable(cfg);

        if (program.isEmpty()) {
            result["success"] = false;
            result["error"] = "Интерпретатор Python не найден в системе!";
            QFile::remove(inputPath);
            return result;
        }

        // Передаем Питону путь к скрипту и пути к JSON-файлам
        args << cfg.entryPoint << inputPath << outputPath;
    }

    // 3. ЗАПУСКАЕМ ПРОЦЕСС
    QProcess process;
    process.setWorkingDirectory(cfg.cachePath);

    qInfo() << "Запуск внешнего процесса:" << program << args;
    process.start(program, args);

    if (!process.waitForFinished(300000)) { // Ждем до 5 минут
        process.kill();
        result["success"] = false;
        result["error"] = "Скрипт завис или выполнялся слишком долго!";
        QFile::remove(inputPath);
        return result;
    }

    if (process.exitCode() != 0) {
        result["success"] = false;

        // ФИКС КРАКОЗЯБР: Читаем поток ошибок консоли с учетом ОС
        QByteArray stderrBytes = process.readAllStandardError();
        QString stderrText;
#if defined(Q_OS_WIN)
        // В Windows консоль обычно выплевывает текст в CP866 (OEM)
        stderrText = QString::fromLocal8Bit(stderrBytes);
#else
        // В Linux/Mac всегда человеческий UTF-8
        stderrText = QString::fromUtf8(stderrBytes);
#endif

        result["error"] = "Ошибка выполнения скрипта:\n" + stderrText;
        QFile::remove(inputPath);
        return result;
    }

    // 4. ЧИТАЕМ РЕЗУЛЬТАТ (output.json)
    QFile outFile(outputPath);
    if (outFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QJsonDocument docOut = QJsonDocument::fromJson(outFile.readAll());
        outFile.close();

        if (docOut.isObject()) {
            result = docOut.object().toVariantMap();
        } else {
            result["success"] = false;
            result["error"] = "Скрипт вернул невалидный JSON.";
        }
    } else {
        result["success"] = false;
        result["error"] = "Скрипт не создал файл output.json (возможно, упал молча).";
    }

    // 5. Уборка
    QFile::remove(inputPath);
    QFile::remove(outputPath);

    return result;
}
QString PluginEngine::getPythonExecutable(const CachedConfig &cfg) const
{
    // 1. В первую очередь проверяем наличие локального venv для плагина
    QString venvPython;
#ifdef Q_OS_WIN
    venvPython = cfg.cachePath + "/.venv/Scripts/python.exe";
#else
    venvPython = cfg.cachePath + "/.venv/bin/python";
#endif

    if (QFile::exists(venvPython)) {
        return venvPython; // Если venv уже создан и существует — работаем через него
    }

    // 2. Откат: Ищем БАЗОВЫЙ портативный Python, который установил Inno Setup
    QString appDir = QCoreApplication::applicationDirPath();
    QString basePython;

#if defined(Q_OS_WIN)
    // Строго папка "python" рядом с нашим .exe в Program Files
    basePython = QDir(appDir).absoluteFilePath("python/python.exe");

    // ХАК ДЛЯ РАЗРАБОТКИ (если запускаешь из Qt Creator, чтобы тоже работало):
    if (!QFile::exists(basePython)) {
        basePython = QDir(appDir).absoluteFilePath("../../installer/win_python/python.exe");
    }
    if (!QFile::exists(basePython)) {
        basePython = QDir(appDir).absoluteFilePath("../../../installer/win_python/python.exe");
    }

    // КРИТИЧЕСКИЙ СЛУЧАЙ: Если портативного питона вообще нигде нет,
    // пробуем найти системный, но ТОЛЬКО через официальный PATH (без жестких путей к AppData)
    if (!QFile::exists(basePython)) {
        basePython = QStandardPaths::findExecutable("python");
        if (basePython.isEmpty()) {
            basePython = "python"; // Самый крайний случай
        }
    }
#elif defined(Q_OS_MAC)
    basePython = QDir(appDir).absoluteFilePath("../Resources/python/bin/python3");
    if (!QFile::exists(basePython)) {
        basePython = QStandardPaths::findExecutable("python3");
        if (basePython.isEmpty()) basePython = QStandardPaths::findExecutable("python");
    }
#else
    // Linux
    basePython = QDir(appDir).absoluteFilePath("python/bin/python3");
    if (!QFile::exists(basePython)) {
        QStringList pythonNames = {"python3", "python"};
        for (const QString &binName : pythonNames) {
            basePython = QStandardPaths::findExecutable(binName);
            if (!basePython.isEmpty()) break;
        }
    }
#endif

    return basePython;
}