#include "finwizard/archivemanager.h"
#include <quazip/quazip.h>
#include <quazip/JlCompress.h>
#include <zlib.h>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <QFileInfo>
#include <QDir>
#include <QDebug>

namespace fs = std::filesystem;

// Внутренняя структура для маппинга вектора памяти под Си-поток microtar
struct MemoryTarStream {
    const char* data;
    size_t size;
    size_t pos;
};

// Подключаем microtar в режиме чистого Си
extern "C" {
#include <microtar.h>
}

// --- Низкоуровневые Си-коллбэки для работы microtar из оперативной памяти ---

static int mem_read(mtar_t* t, void* r_data, unsigned size) {
    auto* s = static_cast<MemoryTarStream*>(t->stream);
    if (s->pos + size > s->size) return MTAR_EFAILURE;
    std::memcpy(r_data, s->data + s->pos, size);
    s->pos += size;
    return MTAR_ESUCCESS;
}

static int mem_seek(mtar_t* t, unsigned offset) {
    auto* s = static_cast<MemoryTarStream*>(t->stream);
    if (offset > s->size) return MTAR_EFAILURE;
    s->pos = offset;
    return MTAR_ESUCCESS;
}

static int mem_close(mtar_t*) {
    return MTAR_ESUCCESS;
}

// --- Реализация методов ArchiveManager ---

bool ArchiveManager::extractArchive(const QString &archivePath, const QString &targetDir)
{
    QFileInfo fi(archivePath);
    QString compExt = fi.completeSuffix().toLower();

    // ========================================================================
    // ЭТАП 1: ВАЛИДАЦИЯ (Сухая проверка структуры БЕЗ создания мусора на диске)
    // ========================================================================
    if (compExt.endsWith("zip")) {
        QuaZip zip(archivePath);
        if (!zip.open(QuaZip::mdUnzip)) {
            qWarning() << "[ArchiveManager] Валидация ZIP провалена:" << zip.getZipError();
            return false;
        }
        zip.close();
    }
    else if (compExt == "tar" || compExt.endsWith("tar.gz") || compExt.endsWith("tgz")) {
        std::vector<char> tarBuffer;

        if (compExt == "tar") {
            std::ifstream in(archivePath.toStdString(), std::ios::binary | std::ios::ate);
            if (!in.is_open()) return false;
            std::streamsize size = in.tellg();
            in.seekg(0, std::ios::beg);
            tarBuffer.resize(size);
            in.read(tarBuffer.data(), size);
            in.close();
        } else {
            tarBuffer = decompressGzip(archivePath.toStdString());
        }

        auto [isValid, errorMsg] = validateTarBuffer(tarBuffer);
        if (!isValid) {
            qWarning() << "[ArchiveManager] Валидация TAR провалена:" << errorMsg;
            return false;
        }
    }
    else {
        qWarning() << "[ArchiveManager] Неподдерживаемый формат архива:" << compExt;
        return false;
    }

    // ========================================================================
    // ЭТАП 2: ФАКТИЧЕСКАЯ РАСПАКОВКА (Сюда дойдем, только если ЭТАП 1 вернул true)
    // ========================================================================
    QDir dir(targetDir);
    if (!dir.exists() && !dir.mkpath(".")) {
        qWarning() << "[ArchiveManager] Не удалось создать директорию:" << targetDir;
        return false;
    }

    bool extractSuccess = false;

    if (compExt.endsWith("zip")) {
        extractSuccess = !JlCompress::extractDir(archivePath, targetDir).isEmpty();
    }
    else if (compExt == "tar") {
        extractSuccess = extractPureTar(archivePath, targetDir);
    }
    else if (compExt.endsWith("tar.gz") || compExt.endsWith("tgz")) {
        extractSuccess = extractTarGz(archivePath, targetDir);
    }

    return extractSuccess;
}

std::vector<char> ArchiveManager::decompressGzip(const std::string& gzFilePath) {
    gzFile file = gzopen(gzFilePath.c_str(), "rb");
    if (!file) return {};

    std::vector<char> buffer;
    char chunk[4096];
    int bytesRead = 0;

    while ((bytesRead = gzread(file, chunk, sizeof(chunk))) > 0) {
        buffer.insert(buffer.end(), chunk, chunk + bytesRead);
    }
    gzclose(file);

    if (bytesRead < 0) return {}; // Ошибка декомпрессии zlib
    return buffer;
}

bool ArchiveManager::extractPureTar(const QString& tarFilePath, const QString& destDir) {
    std::ifstream file(tarFilePath.toStdString(), std::ios::binary | std::ios::ate);
    if (!file.is_open()) return false;

    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<char> buffer(size);
    if (!file.read(buffer.data(), size)) {
        file.close();
        return false;
    }
    file.close();

    return extractTarFromBuffer(buffer, destDir);
}

bool ArchiveManager::extractTarGz(const QString& gzFilePath, const QString& destDir) {
    std::vector<char> decompressedTar = decompressGzip(gzFilePath.toStdString());
    if (decompressedTar.empty()) return false;
    return extractTarFromBuffer(decompressedTar, destDir);
}

QPair<bool, QString> ArchiveManager::validateTarBuffer(const std::vector<char>& tarBuffer) {
    if (tarBuffer.empty()) return {false, "Буфер архива пуст."};

    mtar_t tar;
    std::memset(&tar, 0, sizeof(tar));

    MemoryTarStream stream{ tarBuffer.data(), tarBuffer.size(), 0 };
    tar.stream = &stream;
    tar.read = mem_read;
    tar.seek = mem_seek;
    tar.close = mem_close;

    mtar_header_t header;
    int status;
    int fileCount = 0;

    while ((status = mtar_read_header(&tar, &header)) == MTAR_ESUCCESS) {
        fileCount++;
        status = mtar_next(&tar);
        if (status != MTAR_ESUCCESS) {
            if (status == MTAR_ENULLRECORD) break; // Нормальное окончание TAR
            return {false, QString("Архив поврежден на файле №%1 (%2)").arg(fileCount).arg(header.name)};
        }
    }

    if (status != MTAR_ESUCCESS && status != MTAR_ENULLRECORD) {
        return {false, QString("Ошибка парсинга структуры TAR. Код: %1").arg(status)};
    }
    if (fileCount == 0) return {false, "Архив не содержит файлов."};

    mtar_close(&tar);
    return {true, ""};
}

bool ArchiveManager::extractTarFromBuffer(const std::vector<char>& tarBuffer, const QString& destDir) {
    if (tarBuffer.empty()) return false;

    mtar_t tar;
    std::memset(&tar, 0, sizeof(tar));

    MemoryTarStream stream{ tarBuffer.data(), tarBuffer.size(), 0 };
    tar.stream = &stream;
    tar.read = mem_read;
    tar.seek = mem_seek;
    tar.close = mem_close;

    mtar_header_t header;
    while (mtar_read_header(&tar, &header) == MTAR_ESUCCESS) {
        fs::path targetPath = fs::path(destDir.toStdString()) / header.name;

        if (header.type == MTAR_TDIR) {
            fs::create_directories(targetPath);
        } else if (header.type == MTAR_TREG) {
            if (targetPath.has_parent_path()) {
                fs::create_directories(targetPath.parent_path());
            }

            std::ofstream outFile(targetPath, std::ios::binary);
            if (!outFile.is_open()) return false;

            if (header.size > 0) {
                std::vector<char> fileData(header.size);
                if (mtar_read_data(&tar, fileData.data(), header.size) != MTAR_ESUCCESS) {
                    outFile.close();
                    return false;
                }
                outFile.write(fileData.data(), header.size);
            }
            outFile.close();
        }
        if (mtar_next(&tar) != MTAR_ESUCCESS) break;
    }

    mtar_close(&tar);
    return true;
}