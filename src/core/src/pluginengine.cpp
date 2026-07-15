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
    if (m_execProcess) {
        m_execProcess->disconnect();
        m_execProcess->kill();
        delete m_execProcess;
        m_execProcess = nullptr;
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
        QDir oldVenv(venvPath);
        if (oldVenv.exists()) {
            oldVenv.removeRecursively();
        }
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
    // Нам не нужны setReadChannelEncoding, Qt 6 по умолчанию
    // умеет работать с окружением, а байты мы прочитаем как надо.
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

                                 // Запускаем отложенный скрипт асинхронно — startExternalProcessAsync
                                 // сам эмитит pluginFinished, когда процесс реально завершится.
                                 QMetaObject::invokeMethod(this, [this, cfg]() {
                                     QString startupError;
                                     if (!startExternalProcessAsync(m_delayedCfg, m_delayedParams, startupError)) {
                                         qWarning() << "Не удалось запустить отложенный скрипт:" << startupError;
                                         emit pluginFinished(cfg.id, false, startupError, QString());
                                     }
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

        // Не даем запустить второй внешний процесс, пока первый еще не завершился —
        // иначе они начнут делить один и тот же input_/output_<id>.json.
        if (m_execProcess) {
            QVariantMap busyResult;
            busyResult["success"] = false;
            busyResult["error"] = "Другой плагин уже выполняется. Дождитесь его завершения.";
            return busyResult;
        }

        QString startupError;
        if (!startExternalProcessAsync(cfg, params, startupError)) {
            QVariantMap failResult;
            failResult["success"] = false;
            failResult["error"] = startupError;
            return failResult;
        }

        // Процесс реально запущен и работает в фоне. Финальный результат придет
        // асинхронно через сигнал pluginFinished — раньше runExternalProcess()
        // блокировал этот же поток вызовом waitForFinished(300000) на 5 минут.
        QVariantMap runningResult;
        runningResult["success"] = false;
        runningResult["isRunning"] = true;
        runningResult["error"] = "Плагин запущен и выполняется в фоне...";
        return runningResult;
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
// ---------------- RUN EXTERNAL PROCESS (АСИНХРОННО) -------------------------

bool PluginEngine::startExternalProcessAsync(const CachedConfig &cfg, const QVariantMap &params, QString &startupError)
{
    QString inputPath = cfg.cachePath + QString("/input_%1.json").arg(cfg.id);
    QString outputPath = cfg.cachePath + QString("/output_%1.json").arg(cfg.id);

    // 1. Создаем input.json (ЯВНО СТАВИМ UTF-8 И ТЕКСТОВЫЙ РЕЖИМ)
    QJsonDocument docIn(QJsonObject::fromVariantMap(params));
    QFile inFile(inputPath);
    if (inFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        inFile.write(docIn.toJson(QJsonDocument::Compact));
        inFile.close();
    } else {
        startupError = "Не удалось создать input.json: " + inFile.errorString();
        return false;
    }

    QFile::remove(outputPath); // Удаляем старый результат, если был

    // 2. ОПРЕДЕЛЯЕМ КОМАНДУ ДЛЯ ЗАПУСКА
    QString program;
    QStringList args;

    if (cfg.configType == "executable") {
        program = cfg.entryPoint;
        args << inputPath << outputPath;
    }
    else if (cfg.configType == "python-script") {
        program = getPythonExecutable(cfg);

        if (program.isEmpty()) {
            startupError = "Интерпретатор Python не найден в системе!";
            QFile::remove(inputPath);
            return false;
        }

        QString nativeInputPath = QDir::toNativeSeparators(inputPath);
        QString nativeOutputPath = QDir::toNativeSeparators(outputPath);
        QString nativeEntryPoint = QDir::toNativeSeparators(cfg.entryPoint);

        args << nativeEntryPoint << nativeInputPath << nativeOutputPath;
    }

    // 3. ЗАПУСКАЕМ ПРОЦЕСС АСИНХРОННО
    m_execProcess = new QProcess(this);
    QProcess *proc = m_execProcess; // Локальная копия для безопасного использования в лямбдах
    proc->setWorkingDirectory(cfg.cachePath);

    qInfo() << "Запуск внешнего процесса (асинхронно):" << program << args;

#ifdef Q_OS_WIN
    QProcessEnvironment scriptEnv = QProcessEnvironment::systemEnvironment();
    scriptEnv.insert("PYTHONIOENCODING", "utf-8");
    scriptEnv.insert("PYTHONUTF8", "1");
    proc->setProcessEnvironment(scriptEnv);
#endif

    const QString configType = cfg.configType;
    const int cfgId = cfg.id;

    QObject::connect(proc, &QProcess::errorOccurred, proc, [this, proc, cfgId, inputPath](QProcess::ProcessError error) {
        qWarning() << "Ошибка внешнего процесса плагина:" << error << proc->errorString();

        bool failedToStart = (error == QProcess::FailedToStart);
        QFile::remove(inputPath);

        if (m_execProcess == proc) {
            m_execProcess = nullptr;
        }

        emit pluginFinished(cfgId, false,
                            failedToStart ? "Не удалось запустить процесс: " + proc->errorString()
                                          : "Скрипт завис или упал в процессе выполнения.",
                            QString());
        proc->disconnect();
        proc->deleteLater();
    });

    QObject::connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), proc,
                     [this, proc, cfgId, configType, inputPath, outputPath](int exitCode, QProcess::ExitStatus exitStatus) {

                         QVariantMap result;

                         if (exitStatus != QProcess::NormalExit) {
                             result["success"] = false;
                             result["error"] = "Скрипт завершился аварийно (сбой процесса).";
                         }
                         else if (exitCode != 0) {
                             // ФИКС КРАКОЗЯБР: Читаем поток ошибок консоли с учетом ОС
                             QByteArray stderrBytes = proc->readAllStandardError();
                             QString stderrText = (configType == "python-script")
                                                      ? QString::fromUtf8(stderrBytes)
                                                      : QString::fromLocal8Bit(stderrBytes);

                             result["success"] = false;
                             result["error"] = "Ошибка выполнения скрипта:\n" + stderrText;
                         }
                         else {
                             // Читаем результат (output.json)
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
                         }

                         QFile::remove(inputPath);
                         QFile::remove(outputPath);

                         bool success = result.value("success").toBool();
                         QString msg = result.value("message").toString();
                         QString outPath = result.value("outputPath").toString();
                         if (!success && msg.isEmpty()) {
                             msg = result.value("error").toString();
                         }

                         if (m_execProcess == proc) {
                             m_execProcess = nullptr;
                         }

                         emit pluginFinished(cfgId, success, msg, outPath);
                         proc->deleteLater();
                     });

    proc->start(program, args);

    if (!proc->waitForStarted(5000)) {
        startupError = "Не удалось запустить процесс: " + proc->errorString();
        if (m_execProcess == proc) {
            m_execProcess = nullptr;
        }
        proc->disconnect();
        proc->deleteLater();
        QFile::remove(inputPath);
        return false;
    }

    return true;
}
QString PluginEngine::getPythonExecutable(const CachedConfig &cfg) const
{
    // Безопасный каст для вызова сигналов из const-метода
    PluginEngine* mutableThis = const_cast<PluginEngine*>(this);

    mutableThis->infoLogRequested("=== [PYTHON SEARCH] Начинаю поиск интерпретатора ===");

    QString venvPython;
#ifdef Q_OS_WIN
    venvPython = cfg.cachePath + "/.venv/Scripts/python.exe";
#else
    venvPython = cfg.cachePath + "/.venv/bin/python";
#endif

    mutableThis->infoLogRequested("[PYTHON SEARCH] Шаг 1: Проверка venv плагина: " + QDir::toNativeSeparators(venvPython));
    if (QFile::exists(venvPython)) {
        mutableThis->infoLogRequested("--> Найдено локальное окружение venv плагина.");
        return QDir::toNativeSeparators(venvPython);
    }

    QString appDir = QCoreApplication::applicationDirPath();
    QString basePython;

#if defined(Q_OS_WIN)
    basePython = QDir(appDir).absoluteFilePath("python/python.exe");
    mutableThis->infoLogRequested("[PYTHON SEARCH] Шаг 2 (Win): Проверка портативного пути {app}/python/python.exe: " + QDir::toNativeSeparators(basePython));

    if (!QFile::exists(basePython)) {
        basePython = QDir(appDir).absoluteFilePath("../../installer/win_python/python.exe");
        mutableThis->infoLogRequested("[PYTHON SEARCH] Шаг 2a (Dev-откат 1): Проверка пути разработки: " + QDir::toNativeSeparators(basePython));
    }
    if (!QFile::exists(basePython)) {
        basePython = QDir(appDir).absoluteFilePath("../../../installer/win_python/python.exe");
        mutableThis->infoLogRequested("[PYTHON SEARCH] Шаг 2b (Dev-откат 2): Проверка альтернативного пути разработки: " + QDir::toNativeSeparators(basePython));
    }

    if (!QFile::exists(basePython)) {
        mutableThis->infoLogRequested("[PYTHON SEARCH] Предупреждение: Портативный Python не обнаружен. Ищу в системном PATH...");
        basePython = QStandardPaths::findExecutable("python");
        mutableThis->infoLogRequested("[PYTHON SEARCH] Шаг 3 (Win): Поиск в системном PATH: " + QDir::toNativeSeparators(basePython));

        if (basePython.isEmpty()) {
            basePython = "python";
            mutableThis->infoLogRequested("[PYTHON SEARCH] Внимание: Системный Python не найден в PATH, возвращаю fallback-имя 'python'");
        }
    }
#elif defined(Q_OS_MAC)
    basePython = QDir(appDir).absoluteFilePath("../Resources/python/bin/python3");
    mutableThis->infoLogRequested("[PYTHON SEARCH] Шаг 2 (Mac): Проверка ресурсов бандла: " + QDir::toNativeSeparators(basePython));
    if (!QFile::exists(basePython)) {
        basePython = QStandardPaths::findExecutable("python3");
        if (basePython.isEmpty()) basePython = QStandardPaths::findExecutable("python");
        mutableThis->infoLogRequested("[PYTHON SEARCH] Шаг 3 (Mac): Поиск в системном PATH: " + QDir::toNativeSeparators(basePython));
    }
#else
    basePython = QDir(appDir).absoluteFilePath("python/bin/python3");
    mutableThis->infoLogRequested("[PYTHON SEARCH] Шаг 2 (Linux): Проверка локального пути: " + QDir::toNativeSeparators(basePython));
    if (!QFile::exists(basePython)) {
        QStringList pythonNames = {"python3", "python"};
        for (const QString &binName : pythonNames) {
            basePython = QStandardPaths::findExecutable(binName);
            if (!basePython.isEmpty()) break;
        }
        mutableThis->infoLogRequested("[PYTHON SEARCH] Шаг 3 (Linux): Поиск в системном PATH: " + QDir::toNativeSeparators(basePython));
    }
#endif

    QString finalPath = QDir::toNativeSeparators(basePython);
    mutableThis->infoLogRequested("=== [PYTHON SEARCH] Итоговый выбранный путь: " + finalPath + " ===");
    return finalPath;
}