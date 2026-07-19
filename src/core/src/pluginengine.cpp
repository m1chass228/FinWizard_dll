#include "finwizard/pluginengine.h"
#include <QCoreApplication>
#include <QDebug>
#include <QProcess>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QCoreApplication>
#include <QStandardPaths>
#include <QJsonArray>
#include <QSettings>
#include <QDirIterator>
#include <QTimer>
#include <QRegularExpression>
#include <QTextStream>

namespace {
QProcessEnvironment buildIsolatedPythonEnv(const QString &pythonExePath, const QString &depsDir = QString())
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();

#ifdef Q_OS_WIN
    env.insert("PYTHONIOENCODING", "utf-8");
    env.insert("PYTHONUTF8", "1");
    env.insert("PYTHONLEGACYWINDOWSSTDIO", "1");

    QString pythonDir = QDir::toNativeSeparators(QFileInfo(pythonExePath).absolutePath());
    if (!pythonDir.isEmpty()) {
        QString oldPath = env.value("PATH");
        QString newPath = pythonDir + ";" + pythonDir + "\\Scripts;" + oldPath;
        env.insert("PATH", newPath);
    }

    QString pythonHome = QDir::toNativeSeparators(QFileInfo(pythonExePath).absolutePath());
    env.insert("PYTHONHOME", pythonHome);

    // === ИСПРАВЛЕННЫЙ PYTHONPATH ===
    if (!depsDir.isEmpty()) {
        QString nativeDeps = QDir::toNativeSeparators(depsDir);
        QString pythonPath = nativeDeps;

        qDebug() << "[PYTHON ENV] depsDir =" << nativeDeps;

        // Рекурсивно собираем все директории
        QDirIterator it(depsDir, QDir::Dirs | QDir::NoDotAndDotDot,
                        QDirIterator::Subdirectories | QDirIterator::FollowSymlinks);

        int count = 0;
        while (it.hasNext()) {
            QString subDir = QDir::toNativeSeparators(it.next());
            if (!pythonPath.contains(subDir)) {
                pythonPath += ";" + subDir;
                count++;
            }
        }

        qDebug() << "[PYTHON ENV] Добавлено подпапок:" << count;
        qDebug() << "[PYTHON ENV] Итоговый PYTHONPATH:" << pythonPath;

        env.insert("PYTHONPATH", pythonPath);
    } else {
        qDebug() << "[PYTHON ENV] depsDir пустой!";
        env.remove("PYTHONPATH");
    }

#endif
    return env;
}
} // namespace

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

    // 2. На Windows модифицируем PATH
#ifdef Q_OS_WIN
    QByteArray oldPath = qgetenv("PATH");
    QByteArray pathToAdd = ";" + cacheDir.toUtf8();

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

    // 4. Проверка и превентивная подготовка окружения Python
    CachedConfig dummyCfg;
    dummyCfg.cachePath = cacheDir;
    dummyCfg.id = 0; // Идентификатор для общей подготовки, если критично

    QString depsDir = cacheDir + "/py_deps";
    QString installedMarker = depsDir + "/.requirements_installed";

    if (!QFile::exists(installedMarker)) {
        qInfo() << "[PREPARE] Зависимости плагина еще не установлены. Инициализация сборки...";
        // Вызываем setupPythonEnvironment. Если для этого конфига не требуются зависимости,
        // метод вернет true, иначе запустит асинхронный pip.
        setupPythonEnvironment(dummyCfg);
    }

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
                        QString depLine = dep.toString().trimmed();
                        // Защита от инъекции в requirements.txt: встроенный перевод строки
                        // превращает одну "зависимость" в НЕСКОЛЬКО строк файла, а строка,
                        // начинающаяся с '-', будет прочитана pip как флаг (например,
                        // "--index-url http://evil"), подменяющий источник пакетов.
                        // Манифест — по сути внешние данные (плагин мог прислать кто угодно),
                        // так что валидируем его так же, как любой другой недоверенный ввод.
                        if (depLine.isEmpty()) continue;
                        if (depLine.contains('\n') || depLine.contains('\r') || depLine.startsWith('-')) {
                            qWarning() << "[SECURITY] Отклонена подозрительная строка зависимости в manifest.json:" << depLine;
                            infoLogRequested("[ОШИБКА] Зависимость отклонена (недопустимые символы или флаг): " + depLine.left(60));
                            continue;
                        }
                        stream << depLine << "\n";
                    }
                    reqFile.close();
                    qInfo() << "Requirements.txt успешно сгенерирован автоматически.";
                }
            } else {
                QDir(cfg.cachePath + "/py_deps").mkpath(".");
                QFile marker(cfg.cachePath + "/py_deps/.requirements_installed");
                if (marker.open(QIODevice::WriteOnly)) {
                    marker.close();
                }
                return true;
            }
        }
    }

    // ПЕРЕХОД С VENV НА "--target": зависимости плагина теперь ставятся не в
    // изолированное venv-окружение (со своей копией python.exe), а обычным
    // `pip install --target <depsDir>` от лица ЕДИНСТВЕННОГО базового
    // интерпретатора. Это устраняет целый класс проблем с портативной
    // embeddable-сборкой Python: там нет модуля venv, а откат на virtualenv
    // иногда копирует python3xx.dll, который конфликтует с оригиналом
    // (ImportError: "conflicts with this version of Python"). Раз мы больше
    // никогда не запускаем второй/скопированный python.exe — такой конфликт
    // становится физически невозможен.
    QString depsDir = cfg.cachePath + "/py_deps";
    QString installedMarker = depsDir + "/.requirements_installed";

    if (QFile::exists(installedMarker)) {
        qInfo() << "Зависимости плагина уже установлены, пропускаем pip.";
        return true;
    }

    QString currentPython = findBaseInterpreter();
    if (currentPython.isEmpty()) {
        infoLogRequested("[КРИТИЧЕСКАЯ ОШИБКА] Базовый Python интерпретатор не найден в системе!");
        qWarning() << "Критическая ошибка: Базовый Python интерпретатор не найден!";
        return false;
    }

    // Проверяем физическое наличие файла базового интерпретатора на диске
    if (!QFile::exists(currentPython)) {
        infoLogRequested("[ОШИБКА] Файл базового интерпретатора не существует по пути: " + currentPython);
        qWarning() << "Базовый Python не существует по пути:" << currentPython;
        return false;
    }

    if (!QDir(depsDir).mkpath(".")) {
        infoLogRequested("[ОШИБКА] Не удалось создать папку для зависимостей плагина: " + depsDir);
        qWarning() << "Не удалось создать папку зависимостей:" << depsDir;
        return false;
    }

    QString nativeReqPath = QDir::toNativeSeparators(reqPath);
    QString nativeDepsDir = QDir::toNativeSeparators(depsDir);
    QString nativeBasePython = QDir::toNativeSeparators(currentPython);

    // Считаем количество ПРЯМЫХ зависимостей для грубой оценки прогресса ниже.
    // Это не точный процент (pip дополнительно потянет транзитивные зависимости,
    // на каждую из которых тоже будет своя строка "Collecting" — счетчик может
    // обогнать total), поэтому в проценте ниже жестко ограничиваем потолок 95%,
    // чтобы не показать 100% раньше реального завершения pip.
    int totalPackages = 0;
    {
        QFile reqFileForCount(reqPath);
        if (reqFileForCount.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QTextStream in(&reqFileForCount);
            while (!in.atEnd()) {
                QString line = in.readLine().trimmed();
                if (!line.isEmpty() && !line.startsWith('#')) totalPackages++;
            }
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

    m_pipProcess = new QProcess(this);
    QProcess* proc = m_pipProcess;

    proc->setProcessEnvironment(buildIsolatedPythonEnv(nativeBasePython));

    QStringList pipArgs;
    pipArgs << "-m" << "pip" << "install" << "--target" << nativeDepsDir << "-r" << nativeReqPath;

    // --- КОННЕКТЫ (Контекст жизни — proc) ---

    QObject::connect(proc, &QProcess::readyReadStandardOutput, proc, [this, proc, cfg, totalPackages]() {
        QString output = proc->readAllStandardOutput().trimmed();
        if (!output.isEmpty()) {
            emit pipLogReady(cfg.id, output);

            // Грубый прогресс "N из M пакетов" по строкам "Collecting <pkg>" —
            // не байтовый прогресс скачивания (pip его не печатает без tty),
            // но честно показывает, что процесс двигается, а не завис.
            if (totalPackages > 0) {
                QRegularExpression collectingRe("^\\s*Collecting\\s", QRegularExpression::MultilineOption);
                int newlyCollected = 0;
                auto it = collectingRe.globalMatch(output);
                while (it.hasNext()) { it.next(); newlyCollected++; }

                if (newlyCollected > 0) {
                    int collected = proc->property("fw_collected").toInt() + newlyCollected;
                    proc->setProperty("fw_collected", collected);
                    int percent = qMin(95, static_cast<int>(collected * 100.0 / totalPackages));
                    emit pluginProgress(cfg.id, percent,
                                        QString("Установка зависимостей: %1/%2").arg(qMin(collected, totalPackages)).arg(totalPackages));
                }
            }
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
        infoLogRequested(QString("[ОШИБКА PIP] Процесс pip не смог стартовать/упал: %1").arg(proc->errorString()));
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
                                 qInfo() << "[ДВИЖОК] Запускаю отложенный Python-скрипт...";

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

                         if (m_pipProcess == proc) {
                             m_pipProcess = nullptr;
                         }
                         proc->deleteLater();
                     });

    qInfo() << "Запуск асинхронной установки зависимостей для плагина...";
    proc->start(nativeBasePython, pipArgs);

    if (!proc->waitForStarted(5000)) {
        qWarning() << "Не удалось запустить pip процесс:" << proc->errorString();
        infoLogRequested("[ОШИБКА PIP] Не удалось запустить pip процесс: " + proc->errorString());
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
            QString depsDir = cfg.cachePath + "/py_deps";
            QString installedMarker = depsDir + "/.requirements_installed";

            if (!QFile::exists(installedMarker)) {
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

                qInfo() << "Окружение не готово. Запуск установки зависимостей для конфига:" << cfg.id;

                if (!setupPythonEnvironment(cfg)) {
                    // setupPythonEnvironment провалилась СИНХРОННО (нет Python в системе,
                    // не удалось создать папку зависимостей, не смог стартовать pip-процесс и т.п.).
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
        program = findBaseInterpreter();
        if (program.isEmpty()) {
            startupError = "Интерпретатор Python не найден!";
            QFile::remove(inputPath);
            return false;
        }

        QString depsDir = cfg.cachePath + "/py_deps";
        QString scriptPath = cfg.entryPoint;
        QString bootstrapPath = cfg.cachePath + "/bootstrap.py";

        // === КЕШИРОВАНИЕ BOOTSTRAP ===
        bool bootstrapExists = QFile::exists(bootstrapPath);
        bool depsChanged = false;

        // Простая проверка изменения зависимостей (по маркеру)
        QString markerPath = depsDir + "/.requirements_installed";
        if (bootstrapExists) {
            QFileInfo bootstrapInfo(bootstrapPath);
            QFileInfo markerInfo(markerPath);
            if (markerInfo.exists() && markerInfo.lastModified() > bootstrapInfo.lastModified()) {
                depsChanged = true;
            }
        }

        if (!bootstrapExists || depsChanged) {
            QFile bootstrapFile(bootstrapPath);
            if (bootstrapFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
                QTextStream out(&bootstrapFile);
                out << "import sys\n";
                out << "import os\n";
                out << "import site\n\n";
                out << "# Bootstrap для FinWizard (кешируется)\n";
                // SDK — ищем в двух местах, по приоритету:
                //   1) <cachePath>/python_sdk — плагин принёс СВОЮ копию SDK внутри
                //      архива (например, tar.gz с папкой python_sdk/ рядом с entry-скриптом).
                //      Так плагин самодостаточен и не зависит от того, что доехало
                //      до инсталлятора приложения, и может зафиксировать конкретную
                //      версию SDK, под которую писался.
                //   2) <appDir>/python_sdk — общая версия, поставляемая с самим
                //      приложением, используется как fallback, если плагин свою
                //      копию не принёс.
                // Путь не меняется между релизами SDK (меняется только содержимое
                // файла), поэтому смена версии SDK сама по себе не требует
                // инвалидации кеша bootstrap.py — только смена deps_dir/маркера ниже.
                out << "sdk_dir_shared = r'" << QDir::toNativeSeparators(QCoreApplication::applicationDirPath() + "/python_sdk") << "'\n";
                out << "sdk_dir_bundled = r'" << QDir::toNativeSeparators(cfg.cachePath + "/python_sdk") << "'\n";
                out << "for _sdk_dir in (sdk_dir_shared, sdk_dir_bundled):\n"; // bundled вставляется последним -> выше приоритетом
                out << "    if os.path.exists(_sdk_dir) and _sdk_dir not in sys.path:\n";
                out << "        sys.path.insert(0, _sdk_dir)\n\n";
                out << "deps_dir = r'" << QDir::toNativeSeparators(depsDir) << "'\n";
                out << "if os.path.exists(deps_dir):\n";
                out << "    site.addsitedir(deps_dir)\n";
                out << "    for root, dirs, files in os.walk(deps_dir):\n";
                out << "        if root not in sys.path:\n";
                out << "            sys.path.append(root)\n";
                out << "    print('[BOOTSTRAP] Зависимости загружены (кеш)')\n\n";
                out << "# Запускаем оригинальный скрипт\n";
                out << "script_path = r'" << QDir::toNativeSeparators(scriptPath) << "'\n";
                out << "with open(script_path, encoding='utf-8') as f:\n";
                out << "    exec(f.read())\n";
                bootstrapFile.close();
                qDebug() << "[BOOTSTRAP] Создан/обновлён кеш bootstrap.py";
            }
        } else {
            qDebug() << "[BOOTSTRAP] Используем кешированный bootstrap.py";
        }

        QString nativeBootstrap = QDir::toNativeSeparators(bootstrapPath);
        QString nativeInput = QDir::toNativeSeparators(inputPath);
        QString nativeOutput = QDir::toNativeSeparators(outputPath);

        args << nativeBootstrap << nativeInput << nativeOutput;

        qDebug() << "[BOOTSTRAP] Запуск через" << nativeBootstrap;
    }

    // 3. ЗАПУСКАЕМ ПРОЦЕСС АСИНХРОННО
    m_execProcess = new QProcess(this);
    QProcess *proc = m_execProcess;
    proc->setWorkingDirectory(cfg.cachePath);

    qInfo() << "Запуск внешнего процесса (асинхронно):" << program << args;

    QString depsDirForEnv = (cfg.configType == "python-script") ? (cfg.cachePath + "/py_deps") : QString();
    proc->setProcessEnvironment(buildIsolatedPythonEnv(program, depsDirForEnv));

    // Локальные копии данных из cfg для безопасного захвата по значению в лямбды
    const int cfgId = cfg.id;
    const QString configType = cfg.configType;

    // === RPC-МОСТ: построчный JSON-протокол между плагином и движком ===
    // Отдельный канал от input.json/output.json — только для лёгких сообщений
    // (log, update_progress, get_db_data) во время выполнения. Плагин пишет
    // JSON-объект в свой stdout и синхронно блокируется на stdin в ожидании
    // ответа (см. EngineBridge в finwizard_sdk.py) — поэтому здесь мы обязаны
    // отвечать на КАЖДЫЙ запрос, иначе плагин зависнет навсегда.
    QObject::connect(proc, &QProcess::readyReadStandardOutput, proc, [this, proc, cfgId]() {
        while (proc->canReadLine()) {
            QByteArray line = proc->readLine().trimmed();
            if (line.isEmpty()) continue;

            QJsonParseError parseErr;
            QJsonDocument doc = QJsonDocument::fromJson(line, &parseErr);
            if (parseErr.error != QJsonParseError::NoError || !doc.isObject()) {
                // Не наш протокол (случайный print из плагина/библиотеки,
                // просочившийся мимо _StdoutProxy) — просто логируем, не рвём канал.
                qDebug() << "[BRIDGE] Не-JSON строка от плагина, игнор:" << line;
                continue;
            }

            QJsonObject req = doc.object();
            int reqId = req.value("id").toInt();
            QString action = req.value("action").toString();
            QVariantMap params = req.value("params").toObject().toVariantMap();

            QJsonObject resp{{"id", reqId}};

            if (action == "log") {
                emit pluginLogRequested(cfgId, params.value("message").toString());
                resp["success"] = true;
            } else if (action == "update_progress") {
                emit pluginProgress(cfgId, params.value("percent").toInt(),
                                    params.value("text").toString());
                resp["success"] = true;
            } else if (m_bridgeHandler) {
                // Хук для запросов, специфичных для приложения (get_db_data и т.п.) —
                // устанавливается снаружи через setBridgeHandler(), движок сам
                // ничего не знает о БД. Хендлер должен быть быстрым: плагин
                // блокируется на readline() всё это время.
                QVariantMap result = m_bridgeHandler(cfgId, action, params);
                bool ok = result.value("success", true).toBool();
                resp["success"] = ok;
                if (ok) {
                    resp["result"] = QJsonValue::fromVariant(result.value("result"));
                } else {
                    resp["error"] = result.value("error").toString();
                }
            } else {
                resp["success"] = false;
                resp["error"] = "Unknown action: " + action;
            }

            proc->write(QJsonDocument(resp).toJson(QJsonDocument::Compact) + "\n");
        }
    });

    // === WATCHDOG: раньше зависший процесс ограничивался блокирующим
    // waitForFinished(300000), который убрали ради асинхронности. С мостом
    // появился новый способ зависнуть — плагин ждёт ответа на readline(),
    // который никогда не придёт (баг в диспетчере выше, рассинхрон протокола).
    // Таймер сбрасывается при любой активности на stdout и убивает процесс
    // после затянувшегося молчания.
    auto *watchdog = new QTimer(proc);
    watchdog->setSingleShot(true);
    const int watchdogTimeoutMs = 5 * 60 * 1000; // 5 минут без активности
    QObject::connect(watchdog, &QTimer::timeout, proc, [proc, cfgId]() {
        qWarning() << "[BRIDGE] Плагин" << cfgId << "не отвечает дольше 5 минут, принудительное завершение.";
        proc->kill();
    });
    QObject::connect(proc, &QProcess::readyReadStandardOutput, watchdog, [watchdog, watchdogTimeoutMs]() {
        watchdog->start(watchdogTimeoutMs);
    });
    watchdog->start(watchdogTimeoutMs);

    // Коннект на ошибку запуска/выполнения
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

    // Коннект на успешное или аварийное завершение
    QObject::connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), proc,
                     [this, proc, cfgId, configType, inputPath, outputPath](int exitCode, QProcess::ExitStatus exitStatus) {

                         QVariantMap result;

                         if (exitStatus != QProcess::NormalExit) {
                             result["success"] = false;
                             result["error"] = "Скрипт завершился аварийно (сбой процесса).";
                         }
                         else if (exitCode != 0) {
                             QByteArray stderrBytes = proc->readAllStandardError();
                             QString stderrText = (configType == "python-script")
                                                      ? QString::fromUtf8(stderrBytes)
                                                      : QString::fromLocal8Bit(stderrBytes);

                             result["success"] = false;
                             result["error"] = "Ошибка выполнения скрипта:\n" + stderrText;
                         }
                         else {
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

QString PluginEngine::findBaseInterpreter() const
{
    PluginEngine* mutableThis = const_cast<PluginEngine*>(this);

    mutableThis->infoLogRequested("=== [PYTHON SEARCH] Начинаю поиск интерпретатора Python ===");

    QString appDir = QCoreApplication::applicationDirPath();
    QString basePython;

#if defined(Q_OS_WIN)
    // Шаг 2 (Релиз/Установленная версия): Проверяем папку python рядом с .exe
    basePython = QDir(appDir).absoluteFilePath("python/python.exe");
    mutableThis->infoLogRequested("[PYTHON SEARCH] Шаг 2 (Win): Проверка портативного пути {app}/python/python.exe: " + QDir::toNativeSeparators(basePython));

    // Шаг 2a (Разработка): Если рядом нет, ищем папку проекта поднимаясь вверх по дереву каталогов
    if (!QFile::exists(basePython)) {
        mutableThis->infoLogRequested("[PYTHON SEARCH] Портативный Python рядом с exe не найден. Ищу папку проекта вверх по дереву...");

        QDir searchDir(appDir);

        // Поднимаемся вверх, пока не упремся в корень диска
        while (searchDir.absolutePath() != searchDir.rootPath()) {
            QString potentialPath = searchDir.absoluteFilePath("installer/win_python/python.exe");
            if (QFile::exists(potentialPath)) {
                basePython = potentialPath;
                mutableThis->infoLogRequested("[PYTHON SEARCH] Шаг 2a (Разработка): Успешно найден в папке проекта: " + QDir::toNativeSeparators(basePython));
                break;
            }
            if (!searchDir.cdUp()) {
                break;
            }
        }
    }

    // Шаг 3 (Системный фоллбек): Если папка проекта так и не найдена
    if (!QFile::exists(basePython)) {
        mutableThis->infoLogRequested("[PYTHON SEARCH] Предупреждение: Портативный Python не обнаружен в путях разработки. Ищу в системном PATH...");
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
    mutableThis->infoLogRequested("=== [PYTHON SEARCH] Итоговый выбранный базовый путь: " + finalPath + " ===");
    return finalPath;
}