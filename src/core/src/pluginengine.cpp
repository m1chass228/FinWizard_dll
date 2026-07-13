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
    : QObject(parent), m_pipProcess(nullptr), m_isWaitingForPip(false)
{
}

PluginEngine::~PluginEngine()
{
    if (m_pipProcess) {
        m_pipProcess->disconnect();
        m_pipProcess->kill();
        delete m_pipProcess;
        m_pipProcess = nullptr;
    }
    m_isWaitingForPip = false;
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
            manifestFile.close();
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
                return true;
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

    QString installedMarker = venvPath + "/.requirements_installed";
    if (QFile::exists(venvPython) && QFile::exists(installedMarker)) {
        return true;
    }

    QString basePython = getPythonExecutable(cfg);
    if (basePython.isEmpty() || (!basePython.contains("python") && !QFile::exists(basePython))) {
        qWarning() << "Критическая ошибка: Базовый Python интерпретатор не найден!";
        return false;
    }

    QString nativeVenvPython = QDir::toNativeSeparators(venvPython);
    QString nativeReqPath = QDir::toNativeSeparators(reqPath);
    QString nativeVenvPath = QDir::toNativeSeparators(venvPath);
    QString nativeBasePython = QDir::toNativeSeparators(basePython);

    // 1. Создаем venv синхронно
    if (!QFile::exists(nativeVenvPython)) {
        QProcess createVenv;
#ifdef Q_OS_WIN
        QProcessEnvironment venvEnv = QProcessEnvironment::systemEnvironment();
        venvEnv.insert("PYTHONIOENCODING", "utf-8");
        venvEnv.insert("PYTHONUTF8", "1");
        createVenv.setProcessEnvironment(venvEnv);
#endif
        createVenv.start(nativeBasePython, QStringList() << "-m" << "venv" << nativeVenvPath);
        if (!createVenv.waitForFinished(30000) || createVenv.exitCode() != 0) {
            qWarning() << "Не удалось создать venv:" << createVenv.readAllStandardError();
            return false;
        }
    }

    // 2. АСИНХРОННЫЙ ЗАПУСК PIP
    if (m_pipProcess && m_pipProcess->state() != QProcess::NotRunning) {
        qWarning() << "Pip уже запущен для другого процесса, отмена.";
        return false;
    }

    if (m_pipProcess) {
        m_pipProcess->disconnect();
        m_pipProcess->kill();
        m_pipProcess->deleteLater();
        m_pipProcess = nullptr;
    }

    // Создаем процесс
    m_pipProcess = new QProcess(this);
    QProcess* proc = m_pipProcess; // Локальная копия для безопасного использования в лямбдах

#ifdef Q_OS_WIN
    proc->setReadChannelEncoding(QProcess::OutputChannel::StandardOutput, QStringConverter::Utf8);
    proc->setReadChannelEncoding(QProcess::OutputChannel::StandardError, QStringConverter::Utf8);

    QProcessEnvironment pipEnv = QProcessEnvironment::systemEnvironment();
    pipEnv.insert("PYTHONIOENCODING", "utf-8");
    pipEnv.insert("PYTHONUTF8", "1");
    proc->setProcessEnvironment(pipEnv);
#endif

    QStringList pipArgs;
    pipArgs << "-m" << "pip" << "install" << "-r" << nativeReqPath;

    // --- БЕЗОПАСНЫЕ КОННЕКТЫ (Контекст жизни — сам proc) ---

    QObject::connect(proc, &QProcess::readyReadStandardOutput, proc, [this, proc, cfg]() {
        // Читаем напрямую из захваченного локального указателя, а не из поля класса
        QString output = proc->readAllStandardOutput().trimmed();
        if (!output.isEmpty()) {
            qDebug() << "[PIP OUT]:" << output;
            emit pipLogReady(cfg.id, output);
        }
    });

    QObject::connect(proc, &QProcess::readyReadStandardError, proc, [this, proc, cfg]() {
        QString errorOutput = proc->readAllStandardError().trimmed();
        if (!errorOutput.isEmpty()) {
            qWarning() << "[PIP ERR]:" << errorOutput;
            emit pipLogReady(cfg.id, "[ERROR] " + errorOutput);
        }
    });

    QObject::connect(proc, &QProcess::errorOccurred, proc, [this, proc, cfg](QProcess::ProcessError error) {
        qWarning() << "PIP ошибка процесса:" << error << proc->errorString();

        // Гарантируем, что если это текущий активный процесс движка — сбрасываем флаги
        if (m_pipProcess == proc) {
            m_isWaitingForPip = false;
            m_pipProcess = nullptr;
        }

        emit pipFinished(cfg.id, false);
        proc->disconnect();
        proc->deleteLater();
    });

    QObject::connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), proc,
                     [this, proc, cfg, installedMarker](int exitCode, QProcess::ExitStatus exitStatus) {

                         bool success = (exitStatus == QProcess::NormalExit && exitCode == 0);

                         if (success) {
                             qInfo() << "Установка зависимостей успешно завершена!";
                             QFile marker(installedMarker);
                             if (marker.open(QIODevice::WriteOnly)) {
                                 marker.close();
                             }

                             if (m_isWaitingForPip && m_delayedCfg.id == cfg.id) {
                                 m_isWaitingForPip = false;
                                 qInfo() << "🚀 [ДВИЖОК] Запускаю отложенный Python-скрипт...";

                                 // Чтобы тяжелый runExternalProcess не блокировал калбэк завершения,
                                 // вызываем его через метасистему (асинхронно в следующем тике таймера)
                                 QMetaObject::invokeMethod(this, [this, cfg]() {
                                     QVariantMap runResult = runExternalProcess(m_delayedCfg, m_delayedParams);
                                     qDebug() << "Результат автоматического запуска скрипта движком:" << runResult;

                                     bool scriptSuccess = runResult.value("success").toBool();
                                     QString msg = runResult.value("message").toString();
                                     QString outPath = runResult.value("outputPath").toString();
                                     if (!scriptSuccess && msg.isEmpty()) {
                                         msg = runResult.value("error").toString();
                                     }
                                     emit pluginFinished(cfg.id, scriptSuccess, msg, outPath);
                                 }, Qt::QueuedConnection);

                             } else {
                                 emit pipFinished(cfg.id, true);
                             }
                         } else {
                             qWarning() << "Pip завершился с ошибкой. Код:" << exitCode;
                             if (m_pipProcess == proc) {
                                 m_isWaitingForPip = false;
                             }
                             emit pipFinished(cfg.id, false);
                         }

                         // Зануляем поле класса, только если этот процесс до сих пор является актуальным
                         if (m_pipProcess == proc) {
                             m_pipProcess = nullptr;
                         }
                         proc->deleteLater();
                     });

    qInfo() << "Запуск асинхронной установки зависимостей для плагина...";
    proc->start(nativeVenvPython, pipArgs);

    if (!proc->waitForStarted(5000)) {
        qWarning() << "Не удалось запустить pip процесс:" << proc->errorString();
        if (m_pipProcess == proc) {
            m_pipProcess = nullptr;
        }
        proc->disconnect();
        proc->deleteLater();
        return false;
    }

    return true;
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

    if (!loader->load()) {
        qWarning() << "Ошибка загрузки плагина:" << loader->errorString(); //[cite: 1]
        return false;
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

                if (!setupPythonEnvironment(cfg)) {
                    // setupPythonEnvironment провалилась СИНХРОННО (нет Python в системе,
                    // не удалось создать venv, не смог стартовать pip-процесс и т.п.).
                    // Раньше этот false просто игнорировался: m_isWaitingForPip оставался true
                    // навсегда, никакой сигнал завершения уже не придет (процесс так и не запустился),
                    // и UI зависал на "⏳ Установка библиотек..." без единой ошибки пользователю.
                    m_isWaitingForPip = false;

                    QVariantMap failResult;
                    failResult["success"] = false;
                    failResult["error"] = "Не удалось запустить установку зависимостей Python. Проверьте, установлен ли Python в системе.";
                    return failResult;
                }

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

    QString inputPath = cfg.cachePath + QString("/input_%1.json").arg(cfg.id);
    QString outputPath = cfg.cachePath + QString("/output_%1.json").arg(cfg.id);

    // 1. Создаем input.json (ЯВНО СТАВИМ UTF-8 И ТЕКСТОВЫЙ РЕЖИМ)
    QJsonDocument docIn(QJsonObject::fromVariantMap(params));
    QFile inFile(inputPath);
    if (inFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        inFile.write(docIn.toJson(QJsonDocument::Compact));
        inFile.close();
    } else {
        result["success"] = false;
        result["error"] = "Не удалось создать input.json: " + inFile.errorString();
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
        QString nativeInputPath = QDir::toNativeSeparators(inputPath);
        QString nativeOutputPath = QDir::toNativeSeparators(outputPath);
        // И для скрипта тоже!
        QString nativeEntryPoint = QDir::toNativeSeparators(cfg.entryPoint);

        args << nativeEntryPoint << nativeInputPath << nativeOutputPath;
    }

    // 3. ЗАПУСКАЕМ ПРОЦЕСС
    QProcess process;
    process.setWorkingDirectory(cfg.cachePath);

    qInfo() << "Запуск внешнего процесса:" << program << args;

#ifdef Q_OS_WIN
    QProcessEnvironment scriptEnv = QProcessEnvironment::systemEnvironment();
    scriptEnv.insert("PYTHONIOENCODING", "utf-8");
    scriptEnv.insert("PYTHONUTF8", "1");
    process.setProcessEnvironment(scriptEnv);
#endif
    // ВАЖНО: не используем ForwardedChannels — при нем stdout/stderr дочернего процесса
    // уходят напрямую в консоль родителя мимо внутренних буферов Qt, и readAllStandardError()
    // ниже всегда возвращал бы пустую строку (сообщение об ошибке для пользователя терялось бы).
    // Дефолтный SeparateChannels буферизует оба потока внутри QProcess.
    process.start(program, args);

    if (!process.waitForFinished(300000)) {
        bool failedToStart = (process.error() == QProcess::FailedToStart);
        process.kill();
        result["success"] = false;
        result["error"] = failedToStart
                              ? "Не удалось запустить процесс: " + process.errorString()
                              : "Скрипт завис или выполнялся слишком долго!";
        QFile::remove(inputPath);
        return result;
    }

    if (process.exitCode() != 0) {
        result["success"] = false;

        // ФИКС КРАКОЗЯБР: Читаем поток ошибок консоли с учетом ОС
        QByteArray stderrBytes = process.readAllStandardError();
        QString stderrText;

        if (cfg.configType == "python-script") {
            stderrText = QString::fromUtf8(stderrBytes);
        } else {
            stderrText = QString::fromLocal8Bit(stderrBytes);
        }

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

    return QDir::toNativeSeparators(basePython);
}