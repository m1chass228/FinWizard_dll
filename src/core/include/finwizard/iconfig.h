// iconfig.h
#pragma once

#include <QObject>
#include <QString>
#include <QVariantMap>  // основной способ передачи данных

/**
 * @brief Базовый интерфейс для всех конфигов/плагинов
 *
 * Каждый плагин должен реализовать этот интерфейс.
 * Ядро загружает DLL → проверяет qobject_cast<IConfig*> → вызывает execute()
 */
class IConfig
{

public:
    virtual ~IConfig() = default;

    // Обязательные методы
    virtual QString name() const = 0;                // имя для списка в GUI
    virtual QVariantMap execute(const QVariantMap &params) = 0;  // главная точка входа

    // Опциональные (можно переопределить, если нужно)
    //virtual QString description() const { return QString(); }     // подробное описание
    //virtual QString category() const { return "Разное"; }          // для группировки
    //virtual QString iconName() const { return QString(); }         // имя иконки (из темы или ресурса)
    //virtual QString version() const { return "1.0"; }              // версия плагина
};

Q_DECLARE_INTERFACE(IConfig, "com.finwizard.IConfig/1.0")
