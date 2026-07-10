#ifndef ARCHIVEMANAGER_H
#define ARCHIVEMANAGER_H

#include <QString>
#include <vector>
#include <QPair>

class ArchiveManager {
public:
    // Главная точка входа. Именно её ты будешь вызывать отовсюду!
    // Сначала проверяет архив на целостность, затем извлекает.
    static bool extractArchive(const QString &archivePath, const QString &targetDir);

    // Высокоуровневые методы для работы с TAR (переехали из прошлых наработок)
    static bool extractPureTar(const QString& tarFilePath, const QString& destDir);
    static bool extractTarGz(const QString& gzFilePath, const QString& destDir);
    static QPair<bool, QString> validateTarBuffer(const std::vector<char>& tarBuffer);
    static bool extractTarFromBuffer(const std::vector<char>& tarBuffer, const QString& destDir);

private:
    // Внутренний метод для разжатия Gzip (zlib)
    static std::vector<char> decompressGzip(const std::string& gzFilePath);
};

#endif // ARCHIVEMANAGER_H