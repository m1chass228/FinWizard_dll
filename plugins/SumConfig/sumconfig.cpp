// sumconfig.cpp (внутри плагина)
#include <finwizard/iconfig.h>

class SumConfig : public QObject, public IConfig
{
    Q_OBJECT
    Q_INTERFACES(IConfig)
    Q_PLUGIN_METADATA(IID "com.finwizard.IConfig/1.0")

public:
    QString name() const override { return "Сумма по колонкам"; }
    QString description() const override { return "Суммирует значения в выбранных столбцах"; }

    QVariantMap execute(const QVariantMap &params) override
    {
        QVariantMap result;

        QString xlsxPath = params.value("xlsxPath").toString();
        if (xlsxPath.isEmpty()) {
            result["success"] = false;
            result["error"] = "Не указан путь к файлу";
            return result;
        }

        // Здесь твоя логика: открытие XLSX, суммирование и т.д.
        // ...

        result["success"] = true;
        result["message"] = "Сумма посчитана";
        result["total"] = 123456.78;
        result["processedRows"] = 150;

        return result;
    }
};
