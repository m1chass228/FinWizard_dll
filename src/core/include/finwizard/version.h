#ifndef FINWIZARD_VERSION_H
#define FINWIZARD_VERSION_H

#include <QString>

namespace FinWizard {

// Версия ВСЕГО ПРИЛОЖЕНИЯ — то, что видит пользователь: заголовок окна,
// "О программе", апдейтер и т.п. Меняется при каждом релизе — включая чисто
// косметические правки UI, которые вообще не касаются плагинов.
inline const QString kAppVersion = "2.9.1";

// Версия КОНТРАКТА ДВИЖКА ПЛАГИНОВ — то, против чего PluginRepository
// проверяет "min_engine_version" из manifest.json (см. parseManifest).
// Поднимать ТОЛЬКО когда меняется сам контракт для плагинов: формат
// input.json/output.json, RPC-протокол моста (finwizard_sdk.EngineBridge),
// набор полей манифеста и т.д. — то есть когда старый плагин может реально
// перестать работать с новым движком. Между релизами приложения может
// подолгу не меняться, даже если kAppVersion меняется каждую неделю.
inline const QString kEngineVersion = "1.0.0";

} // namespace FinWizard

#endif // FINWIZARD_VERSION_H
