/*
    SPDX-FileCopyrightText: 2007 Henrique Pinto <henrique.pinto@kdemail.net>
    SPDX-FileCopyrightText: 2008-2009 Harald Hvaal <haraldhv@stud.ntnu.no>
    SPDX-FileCopyrightText: 2010 Raphael Kubo da Costa <rakuco@FreeBSD.org>
    SPDX-FileCopyrightText: 2016 Vladyslav Batyrenko <mvlabat@gmail.com>

    SPDX-License-Identifier: BSD-2-Clause
*/

#include "libarchiveplugin.h"
#include "ark_debug.h"
#include "queries.h"
#include "windows_stat.h"

#include <KLocalizedString>

#include <QDir>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QThread>

#include <archive_entry.h>

LibarchivePlugin::LibarchivePlugin(QObject *parent, const QVariantList &args)
    : ReadWriteArchiveInterface(parent, args)
    , m_archiveReadDisk(archive_read_disk_new())
    , m_cachedArchiveEntryCount(0)
    , m_emitNoEntries(false)
    , m_extractedFilesSize(0)
{
    qCDebug(ARK_LOG) << "Initializing libarchive plugin";
    archive_read_disk_set_standard_lookup(m_archiveReadDisk.data());

    connect(this, &ReadOnlyArchiveInterface::error, this, &LibarchivePlugin::slotRestoreWorkingDir);
    connect(this, &ReadOnlyArchiveInterface::cancelled, this, &LibarchivePlugin::slotRestoreWorkingDir);

#ifdef LIBARCHIVE_RAW_MIMETYPES
    m_rawMimetypes = QStringLiteral(LIBARCHIVE_RAW_MIMETYPES).split(QLatin1Char(':'), Qt::SkipEmptyParts);
    // shared-mime-info 2.3 explicitly separated application/x-bzip2 from application/x-bzip
    // since bzip2 is not compatible with the old (and deprecated) bzip format.
    // See https://gitlab.freedesktop.org/xdg/shared-mime-info/-/merge_requests/239
    // With shared-mime-info 2.3 (or newer) we can't have both mimetypes at the same time, since libarchive does not support
    // the old deprecated bzip format. Also we can't know which version of shared-mime-info the system is actually using.
    // For these reasons, just take the mimetype from QMimeDatabase to keep the compatibility with any shared-mime-info version.
    if (m_rawMimetypes.contains(QLatin1String("application/x-bzip")) && m_rawMimetypes.contains(QLatin1String("application/x-bzip2"))) {
        m_rawMimetypes.removeAll(QLatin1String("application/x-bzip"));
        m_rawMimetypes.removeAll(QLatin1String("application/x-bzip2"));
        m_rawMimetypes.append(QMimeDatabase().mimeTypeForFile(QStringLiteral("dummy.bz2"), QMimeDatabase::MatchExtension).name());
    }
    qCDebug(ARK_LOG) << "# available raw mimetypes:" << m_rawMimetypes.count();
#endif
}

LibarchivePlugin::~LibarchivePlugin()
{
    for (const auto e : std::as_const(m_emittedEntries)) {
        // Entries might be passed to pending slots, so we just schedule their deletion.
        e->deleteLater();
    }
}

bool LibarchivePlugin::list()
{
    qCDebug(ARK_LOG) << "Listing archive contents";

    if (!initializeReader()) {
        return false;
    }

    qCDebug(ARK_LOG) << "Detected compression filter:" << archive_filter_name(m_archiveReader.data(), 0);
    QString compMethod = convertCompressionName(QString::fromUtf8(archive_filter_name(m_archiveReader.data(), 0)));
    if (!compMethod.isEmpty()) {
        Q_EMIT compressionMethodFound(compMethod);
    }

    m_cachedArchiveEntryCount = 0;
    m_extractedFilesSize = 0;
    m_numberOfEntries = 0;
    auto compressedArchiveSize = QFileInfo(filename()).size();

    struct archive_entry *aentry;
    int result = ARCHIVE_RETRY;

    bool firstEntry = true;
    while (!QThread::currentThread()->isInterruptionRequested() && (result = archive_read_next_header(m_archiveReader.data(), &aentry)) == ARCHIVE_OK) {
        if (firstEntry) {
            qCDebug(ARK_LOG) << "Detected format for first entry:" << archive_format_name(m_archiveReader.data());
            firstEntry = false;
        }

        if (!m_emitNoEntries) {
            const bool isRawFormat = (archive_format(m_archiveReader.data()) == ARCHIVE_FORMAT_RAW);
            emitEntryFromArchiveEntry(aentry, isRawFormat);
        }

        m_extractedFilesSize += (qlonglong)archive_entry_size(aentry);

        Q_EMIT progress(float(archive_filter_bytes(m_archiveReader.data(), -1)) / float(compressedArchiveSize));

        m_cachedArchiveEntryCount++;

        // Skip the entry data.
        int readSkipResult = archive_read_data_skip(m_archiveReader.data());
        if (readSkipResult != ARCHIVE_OK) {
            qCCritical(ARK_LOG) << "Error while skipping data for entry:" << QString::fromWCharArray(archive_entry_pathname_w(aentry)) << readSkipResult
                                << QLatin1String(archive_error_string(m_archiveReader.data()));
            if (!emitCorruptArchive()) {
                return false;
            }
        }
    }

    if (QThread::currentThread()->isInterruptionRequested()) {
        return false;
    }

    if (result != ARCHIVE_EOF) {
        qCCritical(ARK_LOG) << "Error while reading archive:" << result << QLatin1String(archive_error_string(m_archiveReader.data()));
        // libarchive currently doesn't support header-encrypted 7zip archives, but doesn't clearly tells when that happens.
        // It just returns a generic ARCHIVE_FATAL when calling the archive_read_next_header function.
        // If there are 0 detected entries, this is likely a header-encrypted archive, but we can't be 100& sure.
        if (archive_format(m_archiveReader.data()) == ARCHIVE_FORMAT_7ZIP && m_cachedArchiveEntryCount == 0) {
            Q_EMIT error(i18nc("@info", "The archive may be corrupt or has header-encryption, which is currently not supported."));
            return false;
        }
        if (!emitCorruptArchive()) {
            return false;
        }
    }

    return archive_read_close(m_archiveReader.data()) == ARCHIVE_OK;
}

bool LibarchivePlugin::emitCorruptArchive()
{
    Kerfuffle::LoadCorruptQuery query(filename());
    Q_EMIT userQuery(&query);
    query.waitForResponse();
    if (!query.responseYes()) {
        Q_EMIT cancelled();
        archive_read_close(m_archiveReader.data());
        return false;
    } else {
        Q_EMIT progress(1.0);
        return true;
    }
}

const QString LibarchivePlugin::uncompressedFileName() const
{
    QFileInfo fileInfo(filename());
    QString uncompressedName(fileInfo.fileName());

    // Bug 252701: For .svgz just remove the terminal "z".
    if (uncompressedName.endsWith(QLatin1String(".svgz"), Qt::CaseInsensitive)) {
        uncompressedName.chop(1);
        return uncompressedName;
    }

    if (!fileInfo.suffix().isEmpty()) {
        return fileInfo.completeBaseName();
    }

    return uncompressedName + QLatin1String(".uncompressed");
}

void LibarchivePlugin::copyDataBlock(const QString &filename, archive *source, archive *dest, bool partialprogress)
{
    while (!QThread::currentThread()->isInterruptionRequested()) {
        const void *buff;
        size_t size;
        la_int64_t offset;
        int returnCode = archive_read_data_block(source, &buff, &size, &offset);
        if (returnCode == ARCHIVE_EOF) {
            return;
        }
        if (returnCode < ARCHIVE_OK) {
            qCCritical(ARK_LOG) << "Error while extracting" << filename << ":" << archive_error_string(source) << "(error no =" << archive_errno(source) << ')';
            return;
        }
        returnCode = archive_write_data_block(dest, buff, size, offset);
        if (returnCode < ARCHIVE_OK) {
            qCCritical(ARK_LOG) << "Error while writing" << filename << ":" << archive_error_string(dest) << "(error no =" << archive_errno(dest) << ')';
            return;
        }
        if (partialprogress) {
            m_currentExtractedFilesSize += size;
            Q_EMIT progress(float(m_currentExtractedFilesSize) / m_extractedFilesSize);
        }
    }
}

bool LibarchivePlugin::addFiles(const QList<Archive::Entry *> &files,
                                const Archive::Entry *destination,
                                const CompressionOptions &options,
                                uint numberOfEntriesToAdd)
{
    Q_UNUSED(files)
    Q_UNUSED(destination)
    Q_UNUSED(options)
    Q_UNUSED(numberOfEntriesToAdd)
    return false;
}

bool LibarchivePlugin::moveFiles(const QList<Archive::Entry *> &files, Archive::Entry *destination, const CompressionOptions &options)
{
    Q_UNUSED(files)
    Q_UNUSED(destination)
    Q_UNUSED(options)
    return false;
}

bool LibarchivePlugin::copyFiles(const QList<Archive::Entry *> &files, Archive::Entry *destination, const CompressionOptions &options)
{
    Q_UNUSED(files)
    Q_UNUSED(destination)
    Q_UNUSED(options)
    return false;
}

bool LibarchivePlugin::deleteFiles(const QList<Archive::Entry *> &files)
{
    Q_UNUSED(files)
    return false;
}

bool LibarchivePlugin::addComment(const QString &comment)
{
    Q_UNUSED(comment)
    return false;
}

bool LibarchivePlugin::testArchive()
{
    return false;
}

bool LibarchivePlugin::hasBatchExtractionProgress() const
{
    return true;
}

bool LibarchivePlugin::doKill()
{
    return false;
}

bool LibarchivePlugin::extractFiles(const QList<Archive::Entry *> &files, const QString &destinationDirectory, const ExtractionOptions &options)
{
    if (!initializeReader()) {
        return false;
    }

    ArchiveWrite writer(archive_write_disk_new());
    if (!writer.data()) {
        return false;
    }

    int totalEntriesCount = 0;
    const bool extractAll = files.isEmpty();
    if (extractAll) {
        if (!m_cachedArchiveEntryCount) {
            Q_EMIT progress(0);
            // TODO: once information progress has been implemented, send
            // feedback here that the archive is being read
            qCDebug(ARK_LOG) << "For getting progress information, the archive will be listed once";
            m_emitNoEntries = true;
            list();
            m_emitNoEntries = false;
        }
        totalEntriesCount = m_cachedArchiveEntryCount;
    } else {
        totalEntriesCount = files.size();
    }

    qCDebug(ARK_LOG) << "Going to extract" << totalEntriesCount << "entries";

    qCDebug(ARK_LOG) << "Changing current directory to " << destinationDirectory;
    m_oldWorkingDir = QDir::currentPath();
    QDir::setCurrent(destinationDirectory);

    // Initialize variables.
    const bool preservePaths = options.preservePaths();
    const bool removeRootNode = options.isDragAndDropEnabled();
    bool overwriteAll = false; // Whether to overwrite all files
    bool skipAll = false; // Whether to skip all files
    bool dontPromptErrors = false; // Whether to prompt for errors
    bool isSingleFile = false;
    m_currentExtractedFilesSize = 0;
    int extractedEntriesCount = 0;
    int progressEntryCount = 0;
    struct archive_entry *entry;
    QString fileBeingRenamed;
    // To avoid traversing the entire archive when extracting a limited set of
    // entries, we maintain a list of remaining entries and stop when it's empty.
    const QStringList fullPaths = entryFullPaths(files);
    QStringList remainingFiles = entryFullPaths(files);

    // Iterate through all entries in archive.
    while (!QThread::currentThread()->isInterruptionRequested() && (archive_read_next_header(m_archiveReader.data(), &entry) == ARCHIVE_OK)) {
        if (!extractAll && remainingFiles.isEmpty()) {
            break;
        }

        fileBeingRenamed.clear();
        int index = -1;

        // Retry with renamed entry, fire an overwrite query again
        // if the new entry also exists.
    retry:
        const bool entryIsDir = S_ISDIR(archive_entry_mode(entry));
        // Skip directories if not preserving paths.
        if (!preservePaths && entryIsDir) {
            archive_read_data_skip(m_archiveReader.data());
            continue;
        }

        // Skip encrypted 7-zip entries, since libarchive cannot extract them yet.
        if (archive_format(m_archiveReader.data()) == ARCHIVE_FORMAT_7ZIP && archive_entry_is_data_encrypted(entry)) {
            archive_read_data_skip(m_archiveReader.data());
            continue;
        }

        // entryName is the name inside the archive, full path
        QString entryName = QDir::fromNativeSeparators(QFile::decodeName(archive_entry_pathname(entry)));
        if (archive_format(m_archiveReader.data()) == ARCHIVE_FORMAT_RAW) {
            isSingleFile = true;
            qCDebug(ARK_LOG) << "Detected single file archive, entry path: " << entryName;
        }
        // Some archive types e.g. AppImage prepend all entries with "./" so remove this part.
        if (entryName.startsWith(QLatin1String("./"))) {
            entryName.remove(0, 2);
        }

        // Static libraries (*.a) contain the two entries "/" and "//".
        // We just skip these to allow extracting this archive type.
        if (entryName == QLatin1String("/") || entryName == QLatin1String("//")) {
            archive_read_data_skip(m_archiveReader.data());
            continue;
        }

        // Don't allow absolute paths, instead, treat them like relative to the root of the archive.
        while (entryName.startsWith(QLatin1Char('/'))) {
            entryName.remove(0, 1);
        }

        // If this ends up empty (e.g. from // or ./), convert to ".".
        if (entryName.isEmpty()) {
            entryName = QStringLiteral(".");
        }

        // Should the entry be extracted?
        if (extractAll || remainingFiles.contains(entryName) || entryName == fileBeingRenamed) {
            // Find the index of entry.
            if (entryName != fileBeingRenamed) {
                index = fullPaths.indexOf(entryName);
            }
            if (!extractAll && index == -1) {
                // If entry is not found in files, skip entry.
                continue;
            }

            // Make sure libarchive uses the same path as we expect, based on transformations and renames,
            qCDebug(ARK_LOG) << "setting path to " << entryName;
            archive_entry_copy_pathname(entry, QFile::encodeName(entryName).constData());
            // entryFI is the fileinfo pointing to where the file will be
            // written from the archive.
            QFileInfo entryFI(entryName);

            if (isSingleFile && fileBeingRenamed.isEmpty()) {
                // Rename extracted file from libarchive-internal "data" name to the archive uncompressed name.
                const QString uncompressedName = uncompressedFileName();
                qCDebug(ARK_LOG) << "going to rename libarchive-internal 'data' filename to:" << uncompressedName;
                archive_entry_copy_pathname(entry, QFile::encodeName(uncompressedName).constData());
                entryFI = QFileInfo(uncompressedName);
            }

            const QString fileWithoutPath(entryFI.fileName());
            // If we DON'T preserve paths, we cut the path and set the entryFI
            // fileinfo to the one without the path.
            if (!preservePaths) {
                // Empty filenames (ie dirs) should have been skipped already,
                // so asserting.
                Q_ASSERT(!fileWithoutPath.isEmpty());
                archive_entry_copy_pathname(entry, QFile::encodeName(fileWithoutPath).constData());
                entryFI = QFileInfo(fileWithoutPath);

                // OR, if the file has a rootNode attached, remove it from file path.
            } else if (!extractAll && removeRootNode && entryName != fileBeingRenamed) {
                const QString &rootNode = files.at(index)->rootNode;
                if (!rootNode.isEmpty()) {
                    const QString truncatedFilename(entryName.remove(entryName.indexOf(rootNode), rootNode.size()));
                    archive_entry_copy_pathname(entry, QFile::encodeName(truncatedFilename).constData());
                    entryFI = QFileInfo(truncatedFilename);
                }
            }

            // Check if the file about to be written already exists.
            if (!entryIsDir && entryFI.exists()) {
                if (skipAll) {
                    archive_read_data_skip(m_archiveReader.data());
                    archive_entry_clear(entry);
                    continue;
                } else if (!overwriteAll && !skipAll) {
                    Kerfuffle::OverwriteQuery query(entryName);
                    Q_EMIT userQuery(&query);
                    query.waitForResponse();

                    if (query.responseCancelled()) {
                        Q_EMIT cancelled();
                        archive_read_data_skip(m_archiveReader.data());
                        archive_entry_clear(entry);
                        break;
                    } else if (query.responseSkip()) {
                        archive_read_data_skip(m_archiveReader.data());
                        archive_entry_clear(entry);
                        continue;
                    } else if (query.responseAutoSkip()) {
                        archive_read_data_skip(m_archiveReader.data());
                        archive_entry_clear(entry);
                        skipAll = true;
                        continue;
                    } else if (query.responseRename()) {
                        const QString newName(query.newFilename());
                        fileBeingRenamed = newName;
                        archive_entry_copy_pathname(entry, QFile::encodeName(newName).constData());
                        goto retry;
                    } else if (query.responseOverwriteAll()) {
                        overwriteAll = true;
                    }
                }
            }

            // If there is an already existing directory.
            if (entryIsDir && entryFI.exists()) {
                if (entryFI.isWritable()) {
                    qCWarning(ARK_LOG) << "Warning, existing, but writable dir";
                } else {
                    qCWarning(ARK_LOG) << "Warning, existing, but non-writable dir. skipping";
                    archive_entry_clear(entry);
                    archive_read_data_skip(m_archiveReader.data());
                    continue;
                }
            }

            int flags = extractionFlags();
            if (archive_entry_sparse_count(entry) > 0) {
                flags |= ARCHIVE_EXTRACT_SPARSE;
            }
            archive_write_disk_set_options(writer.data(), flags);

            // Write the entry header and check return value.
            const int returnCode = archive_write_header(writer.data(), entry);
            switch (returnCode) {
            case ARCHIVE_OK:
                // If the whole archive is extracted and the total filesize is
                // available, we use partial progress.
                copyDataBlock(entryName, m_archiveReader.data(), writer.data(), (extractAll && m_extractedFilesSize));
                break;

            case ARCHIVE_FAILED:
                qCCritical(ARK_LOG) << "archive_write_header() has returned" << returnCode << "with errno" << archive_errno(writer.data());

                // If they user previously decided to ignore future errors,
                // don't bother prompting again.
                if (!dontPromptErrors) {
                    // Ask the user if he wants to continue extraction despite an error for this entry.
                    Kerfuffle::ContinueExtractionQuery query(QLatin1String(archive_error_string(writer.data())), entryName);
                    Q_EMIT userQuery(&query);
                    query.waitForResponse();

                    if (query.responseCancelled()) {
                        Q_EMIT cancelled();
                        return false;
                    }
                    dontPromptErrors = query.dontAskAgain();
                }
                break;

            case ARCHIVE_FATAL:
                qCCritical(ARK_LOG) << "archive_write_header() has returned" << returnCode << "with errno" << archive_errno(writer.data());
                Q_EMIT error(i18nc("@info", "Fatal error, extraction aborted."));
                return false;
            default:
                qCDebug(ARK_LOG) << "archive_write_header() returned" << returnCode << "which will be ignored.";
                break;
            }

            // If we only partially extract the archive and the number of
            // archive entries is available we use a simple progress based on
            // number of items extracted.
            if (!extractAll && m_cachedArchiveEntryCount) {
                ++progressEntryCount;
                Q_EMIT progress(float(progressEntryCount) / totalEntriesCount);
            }

            extractedEntriesCount++;
            remainingFiles.removeOne(entryName);
        } else {
            // Archive entry not among selected files, skip it.
            archive_read_data_skip(m_archiveReader.data());
        }
    }

    // If nothing was extracted, the entries must have been all encrypted.
    if (extractedEntriesCount == 0 && archive_format(m_archiveReader.data()) == ARCHIVE_FORMAT_7ZIP
        && archive_read_has_encrypted_entries(m_archiveReader.data())) {
        Q_EMIT error(i18nc("@info", "Extraction of encrypted 7-zip entries is currently not supported."));
        return false;
    }

    qCDebug(ARK_LOG) << "Extracted" << extractedEntriesCount << "entries";
    slotRestoreWorkingDir();
    return archive_read_close(m_archiveReader.data()) == ARCHIVE_OK;
}

bool LibarchivePlugin::initializeReader()
{
    m_archiveReader.reset(archive_read_new());

    if (!(m_archiveReader.data())) {
        Q_EMIT error(i18n("The archive reader could not be initialized."));
        return false;
    }

    if (archive_read_support_filter_all(m_archiveReader.data()) != ARCHIVE_OK) {
        return false;
    }

    if (m_rawMimetypes.contains(mimetype().name())) {
        qCDebug(ARK_LOG) << "Enabling RAW filter for mimetype: " << mimetype().name();
        // Enable "raw" format only if we have a "raw mimetype", i.e. a single-file archive, as to not affect normal tar archives.
        if (archive_read_support_format_raw(m_archiveReader.data()) != ARCHIVE_OK) {
            return false;
        }
    } else {
        if (archive_read_support_format_all(m_archiveReader.data()) != ARCHIVE_OK) {
            return false;
        }
    }

    if (archive_read_open_filename(m_archiveReader.data(), QFile::encodeName(filename()).constData(), 10240) != ARCHIVE_OK) {
        qCWarning(ARK_LOG) << "Could not open the archive:" << archive_error_string(m_archiveReader.data());
        Q_EMIT error(i18nc("@info", "Archive corrupted or insufficient permissions."));
        return false;
    }

    return true;
}

void LibarchivePlugin::emitEntryFromArchiveEntry(struct archive_entry *aentry, bool isRawFormat)
{
    auto e = new Archive::Entry();
    e->setProperty("fullPath", QDir::fromNativeSeparators(QString::fromWCharArray(archive_entry_pathname_w(aentry))));

    if (isRawFormat) {
        e->setProperty("displayName",
                       uncompressedFileName()); // libarchive reports a fake 'data' entry if raw format, ignore it and use the uncompressed filename.
        e->setProperty("compressedSize", QFileInfo(filename()).size());
        e->compressedSizeIsSet = true;
    } else {
        if (archive_entry_is_encrypted(aentry)) {
            e->setProperty("isPasswordProtected", true);
        }

        const QString owner = QString::fromLatin1(archive_entry_uname(aentry));
        if (!owner.isEmpty()) {
            e->setProperty("owner", owner);
        } else {
            e->setProperty("owner", static_cast<qlonglong>(archive_entry_uid(aentry)));
        }

        const QString group = QString::fromLatin1(archive_entry_gname(aentry));
        if (!group.isEmpty()) {
            e->setProperty("group", group);
        } else {
            e->setProperty("group", static_cast<qlonglong>(archive_entry_gid(aentry)));
        }

        const mode_t mode = archive_entry_mode(aentry);
        if (mode != 0) {
            e->setProperty("permissions", permissionsToString(mode));
        }
        e->setProperty("isExecutable", mode & (S_IXUSR | S_IXGRP | S_IXOTH));

        e->compressedSizeIsSet = false;
        e->setProperty("size", (qlonglong)archive_entry_size(aentry));
        e->setProperty("isDirectory", S_ISDIR(archive_entry_mode(aentry)));

        if (archive_entry_symlink(aentry)) {
            e->setProperty("link", QLatin1String(archive_entry_symlink(aentry)));
        }

        auto time = static_cast<uint>(archive_entry_mtime(aentry));
        e->setProperty("timestamp", QDateTime::fromSecsSinceEpoch(time));
    }

    if (archive_entry_sparse_reset(aentry)) {
        qulonglong sparseSize = 0;
        la_int64_t offset, len;
        while (archive_entry_sparse_next(aentry, &offset, &len) == ARCHIVE_OK) {
            sparseSize += static_cast<qulonglong>(len);
        }
        e->setProperty("sparseSize", sparseSize);
        e->setProperty("isSparse", true);
    }

    Q_EMIT entry(e);
    m_emittedEntries << e;
}

int LibarchivePlugin::extractionFlags() const
{
    return ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_SECURE_NOABSOLUTEPATHS | ARCHIVE_EXTRACT_SECURE_NODOTDOT | ARCHIVE_EXTRACT_SECURE_SYMLINKS;
}

void LibarchivePlugin::copyData(const QString &filename, struct archive *dest, bool partialprogress)
{
    char buff[10240];
    QFile file(filename);

    if (!file.open(QIODevice::ReadOnly)) {
        return;
    }

    auto readBytes = file.read(buff, sizeof(buff));
    while (readBytes > 0 && !QThread::currentThread()->isInterruptionRequested()) {
        archive_write_data(dest, buff, static_cast<size_t>(readBytes));
        if (archive_errno(dest) != ARCHIVE_OK) {
            qCCritical(ARK_LOG) << "Error while writing" << filename << ":" << archive_error_string(dest) << "(error no =" << archive_errno(dest) << ')';
            return;
        }

        if (partialprogress) {
            m_currentExtractedFilesSize += readBytes;
            Q_EMIT progress(float(m_currentExtractedFilesSize) / m_extractedFilesSize);
        }

        readBytes = file.read(buff, sizeof(buff));
    }

    file.close();
}

void LibarchivePlugin::copyData(const QString &filename, struct archive *source, struct archive *dest, bool partialprogress)
{
    char buff[10240];

    auto readBytes = archive_read_data(source, buff, sizeof(buff));
    while (readBytes > 0 && !QThread::currentThread()->isInterruptionRequested()) {
        archive_write_data(dest, buff, static_cast<size_t>(readBytes));
        if (archive_errno(dest) != ARCHIVE_OK) {
            qCCritical(ARK_LOG) << "Error while extracting" << filename << ":" << archive_error_string(dest) << "(error no =" << archive_errno(dest) << ')';
            return;
        }

        if (partialprogress) {
            m_currentExtractedFilesSize += readBytes;
            Q_EMIT progress(float(m_currentExtractedFilesSize) / m_extractedFilesSize);
        }

        readBytes = archive_read_data(source, buff, sizeof(buff));
    }
}

void LibarchivePlugin::slotRestoreWorkingDir()
{
    if (m_oldWorkingDir.isEmpty()) {
        return;
    }

    if (!QDir::setCurrent(m_oldWorkingDir)) {
        qCWarning(ARK_LOG) << "Failed to restore old working directory:" << m_oldWorkingDir;
    } else {
        m_oldWorkingDir.clear();
    }
}

QString LibarchivePlugin::convertCompressionName(const QString &method)
{
    if (method == QLatin1String("gzip")) {
        return QStringLiteral("GZip");
    } else if (method == QLatin1String("bzip2")) {
        return QStringLiteral("BZip2");
    } else if (method == QLatin1String("xz")) {
        return QStringLiteral("XZ");
    } else if (method == QLatin1String("compress (.Z)")) {
        return QStringLiteral("Compress");
    } else if (method == QLatin1String("lrzip")) {
        return QStringLiteral("LRZip");
    } else if (method == QLatin1String("lzip")) {
        return QStringLiteral("LZip");
    } else if (method == QLatin1String("lz4")) {
        return QStringLiteral("LZ4");
    } else if (method == QLatin1String("lzop")) {
        return QStringLiteral("lzop");
    } else if (method == QLatin1String("lzma")) {
        return QStringLiteral("LZMA");
    } else if (method == QLatin1String("zstd")) {
        return QStringLiteral("Zstandard");
    }
    return QString();
}

#include "moc_libarchiveplugin.cpp"
