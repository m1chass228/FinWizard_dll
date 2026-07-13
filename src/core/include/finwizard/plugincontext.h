#ifndef PLUGINCONTEXT_H
#define PLUGINCONTEXT_H

#endif // PLUGINCONTEXT_H

#pragma once

#include <QObject>
#include <QJsonObject>
#include <QString>
#include <QVariant>
#include "ifinwizardcontext.h"

class PluginContext : public QObject, public IFinWizardContext {
    Q_OBJECT
public:
    explicit PluginContext(int pluginId, const QString& inputPath, const QJsonObject& userInputs, QObject *parent = nullptr);
    ~PluginContext() override;

    // --- Реализация интерфейса IFinWizardContext (машинный код / DLL) ---
    void setProgress(int percentage) override;
    void finish(bool success, const char* errorMessage) override;
    bool isCancellationRequested() const override;

    void logInfo(const char* message) override;
    void logError(const char* message) override;
    void showAlert(const char* title, const char* text, const char* type) override;

    const char* getInputPath() override;
    const char* createTempDir() override;
    bool shouldUnpack() const override;

    const char* getArgumentString(const char* key) override;
    bool getArgumentBool(const char* key) override;
    int getArgumentInt(const char* key) override;

    // --- Q_INVOKABLE методы для скриптовых языков (Python QProcess / JS WebChannel) ---
    Q_INVOKABLE void setProgress(int percentage) { setProgress(percentage); }
    Q_INVOKABLE void finish(bool success, const QString& msg) { finish(success, msg.toUtf8().constData()); }
    Q_INVOKABLE void logInfo(const QString& msg) { logInfo(msg.toUtf8().constData()); }
    Q_INVOKABLE void logError(const QString& msg) { logError(msg.toUtf8().constData()); }
    Q_INVOKABLE QString getInputPathStr() const { return m_inputPath; }
    Q_INVOKABLE QVariant getArgument(const QString& key) const;

signals:
    // Эти сигналы свяжут контекст с PluginEngine и MainWindow
    void progressUpdated(int pluginId, int percentage);
    void logReceived(int pluginId, const QString& text, bool isError);
    void alertRequested(const QString& title, const QString& text, const QString& type);
    void executionFinished(int pluginId, bool success, const QString& msg);

private:
    int m_pluginId;
    QString m_inputPath;
    QJsonObject m_userInputs;
    bool m_cancelRequested;

    // Кэш для возврата const char* в DLL без утечек памяти
    mutable QByteArray m_cachedInputPath;
    mutable QByteArray m_cachedTempDir;
    mutable QByteArray m_cachedArgString;
};