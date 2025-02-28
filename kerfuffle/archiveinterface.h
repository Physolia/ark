/*
    SPDX-FileCopyrightText: 2007 Henrique Pinto <henrique.pinto@kdemail.net>
    SPDX-FileCopyrightText: 2008-2009 Harald Hvaal <haraldhv@stud.ntnu.no>
    SPDX-FileCopyrightText: 2009-2012 Raphael Kubo da Costa <rakuco@FreeBSD.org>
    SPDX-FileCopyrightText: 2016 Vladyslav Batyrenko <mvlabat@gmail.com>

    SPDX-License-Identifier: BSD-2-Clause
*/

#ifndef ARCHIVEINTERFACE_H
#define ARCHIVEINTERFACE_H

#include "archive_kerfuffle.h"
#include "archiveentry.h"
#include "kerfuffle_export.h"

#include <KPluginMetaData>

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <qplatformdefs.h>

namespace Kerfuffle
{
class Query;

enum {
    PossiblyMaliciousArchiveError = KJob::UserDefinedError + 1,
    DestinationNotWritableError,
};

class KERFUFFLE_EXPORT ReadOnlyArchiveInterface : public QObject
{
    Q_OBJECT
public:
    explicit ReadOnlyArchiveInterface(QObject *parent, const QVariantList &args);
    ~ReadOnlyArchiveInterface() override;

    /**
     * Returns the filename of the archive currently being handled.
     */
    QString filename() const;

    /**
     * Returns the comment of the archive.
     */
    QString comment() const;

    /**
     * @return The password of the archive, if any.
     */
    QString password() const;

    bool isMultiVolume() const;
    int numberOfVolumes() const;

    /**
     * Returns whether the file can only be read.
     *
     * @return @c true  The file cannot be written.
     * @return @c false The file can be read and written.
     */
    virtual bool isReadOnly() const;

    virtual bool open();

    /**
     * List archive contents.
     * This runs the process of reading archive contents.
     * When subclassing, you can block as long as you need (unless you called setWaitForFinishedSignal(true)).
     * @returns whether the listing succeeded.
     * @note If returning false, make sure to emit the error() signal beforewards to notify
     * the user of the error condition.
     */
    virtual bool list() = 0;
    virtual bool testArchive() = 0;
    void setPassword(const QString &password);
    void setHeaderEncryptionEnabled(bool enabled);

    /**
     * Extracts the given @p files to the given @p destinationDirectory.
     * If @p files is empty, the whole archive will be extracted.
     * When subclassing, you can block as long as you need (unless you called setWaitForFinishedSignal(true)).
     * @returns whether the extraction succeeded.
     * @note If returning false, make sure to emit the error() signal beforewards to notify
     * the user of the error condition.
     */
    virtual bool extractFiles(const QList<Archive::Entry *> &files, const QString &destinationDirectory, const ExtractionOptions &options) = 0;

    /**
     * @return Whether the plugins do NOT run the functions in their own thread.
     * @see setWaitForFinishedSignal()
     */
    bool waitForFinishedSignal();

    /**
     * Returns count of required finish signals for a job to be finished.
     *
     * These two methods are used by move and copy jobs, which in some plugins implementations have to call
     * several processes sequentially. For instance, moving entries in zip archive is only possible if
     * extracting the entries, deleting them, recreating destination folder structure and adding them back again.
     */
    virtual int moveRequiredSignals() const;
    virtual int copyRequiredSignals() const;

    /**
     * Returns the list of filenames retrieved from the list of entries.
     */
    static QStringList entryFullPaths(const QList<Archive::Entry *> &entries, PathFormat format = WithTrailingSlash);

    /**
     * Returns the list of the entries, excluding their children.
     *
     * This method relies on entries paths so doesn't require parents to be set.
     */
    static QList<Archive::Entry *> entriesWithoutChildren(const QList<Archive::Entry *> &entries);

    /**
     * Returns the string list of entry paths, which will be a result of adding/moving/copying entries.
     *
     * @param entries The entries which will be added/moved/copied.
     * @param destination Destination path within the archive to which entries have to be added. For renaming an entry
     * the path has to contain a new filename too.
     * @param entriesWithoutChildren Entries count, excluding their children. For AddJob or CopyJob 0 MUST be passed.
     *
     * @return For entries
     *  some/dir/
     *  some/dir/entry
     *  some/dir/some/entry
     *  some/another/entry
     * and destination
     *  some/destination
     * will return
     *  some/destination/dir/
     *  some/destination/dir/entry
     *  some/destination/dir/some/enty
     *  some/destination/entry
     */
    static QStringList entryPathsFromDestination(QStringList entries, const Archive::Entry *destination, int entriesWithoutChildren);

    /**
     * @return true if the interface has killed the job.
     * Otherwise returns false if the interface is not able to instantly kill the operation.
     */
    virtual bool doKill();

    bool isHeaderEncryptionEnabled() const;
    virtual QString multiVolumeName() const;
    void setMultiVolume(bool value);
    uint numberOfEntries() const;
    QMimeType mimetype() const;

    /**
     * @return Whether the interface supports progress reporting for BatchExtractJobs.
     */
    virtual bool hasBatchExtractionProgress() const;

    /**
     * @return Whether the archive is locked (RAR feature).
     */
    virtual bool isLocked() const;

    /**
     * Returns the size of the unpacked archive
     */
    qulonglong unpackedSize() const;

    static QString permissionsToString(mode_t perm);

Q_SIGNALS:

    /**
     * Emitted when the user cancels the operation. Examples:
     * - the user cancels the password dialog
     * - the user cancels the overwrite dialog
     */
    void cancelled();
    void error(const QString &message, const QString &details = QString(), int errorCode = KJob::UserDefinedError);
    void entry(Kerfuffle::Archive::Entry *archiveEntry);
    void progress(double progress);
    void info(const QString &info);
    void finished(bool result);
    void testSuccess();
    void compressionMethodFound(const QString &method);
    void encryptionMethodFound(const QString &method);

    /**
     * Emitted when @p query needs to be executed on the GUI thread.
     */
    void userQuery(Kerfuffle::Query *query);

protected:
    /**
     * Setting this option to true will NOT run the functions in their own thread.
     * Instead it will be necessary to call finished(bool) when the operation is actually finished.
     */
    void setWaitForFinishedSignal(bool value);

    void setCorrupt(bool isCorrupt);
    bool isCorrupt() const;
    QString m_comment;
    int m_numberOfVolumes;
    uint m_numberOfEntries;
    KPluginMetaData m_metaData;

private:
    QString m_filename;
    QMimeType m_mimetype;
    QString m_password;
    bool m_waitForFinishedSignal;
    bool m_isHeaderEncryptionEnabled;
    bool m_isCorrupt;
    bool m_isMultiVolume;
    qulonglong m_unpackedSize;

private Q_SLOTS:
    void onEntry(Kerfuffle::Archive::Entry *archiveEntry);
};

class KERFUFFLE_EXPORT ReadWriteArchiveInterface : public ReadOnlyArchiveInterface
{
    Q_OBJECT
public:
    enum OperationMode {
        NoOperation,
        List,
        Extract,
        Add,
        Move,
        Copy,
        Delete,
        Comment,
        Test,
    };

    explicit ReadWriteArchiveInterface(QObject *parent, const QVariantList &args);
    ~ReadWriteArchiveInterface() override;

    bool isReadOnly() const override;

    /**
     * Adds the given @p files under the given @p destination.
     * If @p destination is null, the files will be added under the root of the archive.
     * @param options The compression options that must be respected.
     * @param numberOfEntriesToAdd The total number of entries the will be added.
     * @return Whether the operation succeeded.
     * @note If returning false, make sure to emit the error() signal beforewards to notify
     * the user of the error condition.
     */
    virtual bool
    addFiles(const QList<Archive::Entry *> &files, const Archive::Entry *destination, const CompressionOptions &options, uint numberOfEntriesToAdd = 0) = 0;
    virtual bool moveFiles(const QList<Archive::Entry *> &files, Archive::Entry *destination, const CompressionOptions &options) = 0;
    virtual bool copyFiles(const QList<Archive::Entry *> &files, Archive::Entry *destination, const CompressionOptions &options) = 0;
    virtual bool deleteFiles(const QList<Archive::Entry *> &files) = 0;
    virtual bool addComment(const QString &comment) = 0;

Q_SIGNALS:
    void entryRemoved(const QString &path);

protected:
    OperationMode m_operationMode = NoOperation;

private Q_SLOTS:
    void onEntryRemoved(const QString &path);
};

} // namespace Kerfuffle

#endif // ARCHIVEINTERFACE_H
