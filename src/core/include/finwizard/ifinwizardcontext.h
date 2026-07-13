#ifndef IFINWIZARDCONTEXT_H
#define IFINWIZARDCONTEXT_H

#endif // IFINWIZARDCONTEXT_H

#pragma once

// Чистый C++ интерфейс, абстрагированный от Qt для бинарной совместимости DLL
class IFinWizardContext {
public:
    virtual ~IFinWizardContext() = default;

    // --- Core API ---
    virtual void setProgress(int percentage) = 0;
    virtual void finish(bool success, const char* errorMessage) = 0;
    virtual bool isCancellationRequested() const = 0;

    // --- UI & Logging ---
    virtual void logInfo(const char* message) = 0;
    virtual void logError(const char* message) = 0;
    virtual void showAlert(const char* title, const char* text, const char* type) = 0;

    // --- File System & Config ---
    virtual const char* getInputPath() = 0;
    virtual const char* createTempDir() = 0;
    virtual bool shouldUnpack() const = 0;

    // --- Arguments API ---
    virtual const char* getArgumentString(const char* key) = 0;
    virtual bool getArgumentBool(const char* key) = 0;
    virtual int getArgumentInt(const char* key) = 0;
};

// Тип функции входа, которую обязана экспортировать каждая DLL
typedef void (*PluginMainFunc)(IFinWizardContext* ctx);