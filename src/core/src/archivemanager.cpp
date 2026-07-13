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
    // Вычитание вместо сложения: не переполняется даже если pos близок к максимуму size_t
    if (s->pos > s->size || size > s->size - s->pos) return MTAR_EFAILURE;
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

// --- Защита от path traversal ("tar slip"): вредоносный архив может содержать
// имена вида "../../etc/passwd" или абсолютные пути, которые без проверки
// уводят запись за пределы целевой директории. ---
static bool safeJoinTarPath(const fs::path& baseDir, const std::string& entryName, fs::path& outPath) {
    if (entryName.empty()) return false;

    fs::path rel(entryName);
    if (rel.is_absolute()) return false;

    fs::path base = baseDir.lexically_normal();
    fs::path candidate = (base / rel).lexically_normal();

    // Приводим оба пути к строкам для честного сравнения префикса
    const std::string baseStr = base.string();
    const std::string candStr = candidate.string();

    if (candStr.size() < baseStr.size() || candStr.compare(0, baseStr.size(), baseStr) != 0) {
        return false; // ушли выше baseDir через "../.."
    }
    // Защита от ложного совпадения префикса без разделителя (base=/tmp/out, candidate=/tmp/outEvil)
    if (candStr.size() > baseStr.size()) {
        char sep = candStr[baseStr.size()];
        if (sep != '/' && sep != fs::path::preferred_separator) return false;
    }

    outPath = candidate;
    return true;
}

// --- Реализация методов ArchiveManager ---

bool ArchiveManager::extractArchive(const QString &archivePath, const QString &targetDir)
{
    QFileInfo fi(archivePath);
    QString compExt = fi.completeSuffix().toLower();

    // Буфер TAR, прочитанный и провалидированный на ЭТАПЕ 1. Переиспользуем его же
    // на ЭТАПЕ 2, чтобы не читать файл с диска второй раз (TOCTOU: между двумя чтениями
    // содержимое файла могло измениться) и не декомпрессировать gzip повторно.
    std::vector<char> tarBuffer;

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
    else {
        // TAR / TAR.GZ / TGZ — используем тот же буфер, что и на этапе валидации
        extractSuccess = extractTarFromBuffer(tarBuffer, targetDir);
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
            mtar_close(&tar);
            return {false, QString("Архив поврежден на файле №%1 (%2)").arg(fileCount).arg(header.name)};
        }
    }

    if (status != MTAR_ESUCCESS && status != MTAR_ENULLRECORD) {
        mtar_close(&tar);
        return {false, QString("Ошибка парсинга структуры TAR. Код: %1").arg(status)};
    }
    if (fileCount == 0) {
        mtar_close(&tar);
        return {false, "Архив не содержит файлов."};
    }

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

    const fs::path baseDir = fs::path(destDir.toStdString());

    mtar_header_t header;
    int status;
    while ((status = mtar_read_header(&tar, &header)) == MTAR_ESUCCESS) {
        fs::path targetPath;
        if (!safeJoinTarPath(baseDir, header.name, targetPath)) {
            qWarning() << "[ArchiveManager] Обнаружена попытка выхода за пределы целевой папки (path traversal), запись отклонена:" << header.name;
            mtar_close(&tar);
            return false;
        }

        if (header.type == MTAR_TDIR) {
            fs::create_directories(targetPath);
        } else if (header.type == MTAR_TREG) {
            if (targetPath.has_parent_path()) {
                fs::create_directories(targetPath.parent_path());
            }

            std::ofstream outFile(targetPath, std::ios::binary);
            if (!outFile.is_open()) {
                mtar_close(&tar);
                return false;
            }

            if (header.size > 0) {
                std::vector<char> fileData(header.size);
                if (mtar_read_data(&tar, fileData.data(), header.size) != MTAR_ESUCCESS) {
                    outFile.close();
                    mtar_close(&tar);
                    return false;
                }
                outFile.write(fileData.data(), header.size);
            }
            outFile.close();
        }

        status = mtar_next(&tar);
        if (status != MTAR_ESUCCESS) break;
    }

    mtar_close(&tar);

    // Различаем нормальный конец архива (MTAR_ENULLRECORD) от реального сбоя парсинга —
    // раньше функция в обоих случаях молча возвращала true.
    if (status != MTAR_ESUCCESS && status != MTAR_ENULLRECORD) {
        qWarning() << "[ArchiveManager] Архив поврежден в процессе распаковки. Код:" << status;
        return false;
    }

    return true;
}