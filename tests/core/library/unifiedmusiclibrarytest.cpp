/*
 * Fooyin
 * Copyright © 2026, Luke Taylor <luket@pm.me>
 *
 * Fooyin is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Fooyin is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Fooyin.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "testutils.h"

#include "core/database/dbschema.h"
#include "core/database/trackdatabase.h"
#include "core/internalcoresettings.h"
#include "core/library/librarymanager.h"
#include "core/library/libraryscanner.h"
#include "core/library/unifiedmusiclibrary.h"
#include "core/network/networkaccessmanager.h"
#include "core/network/remoteioservice.h"
#include "core/playlist/parsers/cueparser.h"
#include "core/playlist/playlisthandler.h"
#include "core/playlist/playlistloader.h"
#ifdef FOOYIN_TEST_TAGEDITOR
#include "settings/tageditorfieldregistry.h"
#include "tageditoritem.h"
#include "tageditormodel.h"
#include "tageditorpropertiestab.h"
#include "tageditorsettings.h"
#include "tageditorstats.h"
#endif
#include <core/coresettings.h>
#include <core/library/pendingtrackcoverprovider.h>
#include <core/player/playercontroller.h>

#include <core/engine/audioloader.h>
#include <core/engine/input/ratingtagpolicy.h>
#include <utils/database/dbconnectionhandler.h>
#include <utils/database/dbconnectionpool.h>
#include <utils/database/dbconnectionprovider.h>
#include <utils/database/dbquery.h>
#include <utils/settings/settingsmanager.h>
#ifdef FOOYIN_TEST_TAGEDITOR
#include <core/constants.h>
#include <gui/guisettings.h>
#include <utils/actions/actionmanager.h>

#include <QApplication>
#include <QMainWindow>
#include <QTimer>
#endif

#include <QBuffer>
#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSignalBlocker>
#include <QSignalSpy>
#include <QSqlError>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QThreadPool>
#include <QTimeZone>
#include <QtConcurrent/QtConcurrentRun>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <ranges>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace Qt::StringLiterals;

constexpr auto CurrentSchemaVersion = 22;

namespace {
QCoreApplication* ensureCoreApplication()
{
    QStandardPaths::setTestModeEnabled(true);

    if(auto* app = QCoreApplication::instance()) {
        return app;
    }

    static int argc{1};
    static char appName[]        = "fooyin-unifiedmusiclibrary-test";
    static char* argv[]          = {appName, nullptr};
    static QCoreApplication* app = []() {
#ifdef FOOYIN_TEST_TAGEDITOR
        if(qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
            qputenv("QT_QPA_PLATFORM", "offscreen");
        }
        auto* instance = new QApplication(argc, argv);
#else
        auto* instance = new QCoreApplication(argc, argv);
#endif
        QCoreApplication::setApplicationName(QString::fromLatin1(appName));
        return instance;
    }();
    return app;
}

void initialiseTestEnvironment()
{
    static const bool initialised = []() {
        ensureCoreApplication();
        QLoggingCategory::setFilterRules(u"fy.db.info=false\nfy.library.info=false\nfy.scanner.info=false"_s);
        return true;
    }();

    static_cast<void>(initialised);
}

template <typename Predicate>
bool waitForCondition(Predicate&& predicate, int timeoutMs = 10000)
{
    QElapsedTimer timer;
    timer.start();

    while(timer.elapsed() < timeoutMs) {
        if(predicate()) {
            return true;
        }

        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }

    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return predicate();
}

Fooyin::DbConnectionPoolPtr createTestDbPool(const QString& dbPath)
{
    static std::atomic_int counter{0};

    Fooyin::DbConnection::DbParams params;
    params.type           = u"QSQLITE"_s;
    params.connectOptions = u"QSQLITE_OPEN_URI"_s;
    params.filePath       = dbPath;

    const auto connectionName = u"fooyin-unifiedmusiclibrary-test-%1"_s.arg(counter.fetch_add(1));
    auto dbPool               = Fooyin::DbConnectionPool::create(params, connectionName);

    const Fooyin::DbConnectionHandler handler{dbPool};
    EXPECT_TRUE(handler.hasConnection());

    Fooyin::DbSchema schema{Fooyin::DbConnectionProvider{dbPool}};
    const auto upgradeResult = schema.upgradeDatabase(CurrentSchemaVersion, u"://dbschema.xml"_s);

    EXPECT_TRUE(upgradeResult == Fooyin::DbSchema::UpgradeResult::Success
                || upgradeResult == Fooyin::DbSchema::UpgradeResult::IsCurrent
                || upgradeResult == Fooyin::DbSchema::UpgradeResult::BackwardsCompatible);
    return dbPool;
}

void requireWorkingDatabase(const Fooyin::DbConnectionPoolPtr& dbPool)
{
    const Fooyin::DbConnectionProvider dbProvider{dbPool};
    Fooyin::DbQuery probeQuery{dbProvider.db(), u"SELECT 1;"_s};
    if(!probeQuery.exec()) {
        GTEST_SKIP() << probeQuery.lastError().text().toStdString();
    }
}

int insertLibrary(Fooyin::DbConnectionPoolPtr dbPool, const QString& path, const QString& name)
{
    const Fooyin::DbConnectionProvider dbProvider{std::move(dbPool)};

    Fooyin::DbQuery insertQuery{dbProvider.db(), u"INSERT INTO Libraries (Name, Path) VALUES (:name, :path);"_s};
    insertQuery.bindValue(u":name"_s, name);
    insertQuery.bindValue(u":path"_s, path);
    EXPECT_TRUE(insertQuery.exec()) << insertQuery.lastError().text().toStdString();

    Fooyin::DbQuery selectQuery{dbProvider.db(), u"SELECT LibraryID FROM Libraries WHERE Path = :path;"_s};
    selectQuery.bindValue(u":path"_s, path);
    EXPECT_TRUE(selectQuery.exec()) << selectQuery.lastError().text().toStdString();
    EXPECT_TRUE(selectQuery.next()) << selectQuery.lastError().text().toStdString();

    return selectQuery.value(0).toInt();
}

void writeFile(const QString& path, const QByteArray& data = "test")
{
    QFile file{path};
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    ASSERT_EQ(file.write(data), data.size());
}

QStringList sortedTrackTitles(const Fooyin::TrackList& tracks)
{
    QStringList titles;
    titles.reserve(static_cast<qsizetype>(tracks.size()));

    for(const auto& track : tracks) {
        titles.push_back(track.title());
    }

    std::ranges::sort(titles);
    return titles;
}

QStringList trackFileNames(const Fooyin::TrackList& tracks)
{
    QStringList fileNames;
    fileNames.reserve(static_cast<qsizetype>(tracks.size()));

    for(const auto& track : tracks) {
        fileNames.push_back(QFileInfo{track.filepath()}.fileName());
    }

    return fileNames;
}

QStringList activeTrackTitles(const Fooyin::TrackList& tracks)
{
    QStringList titles;
    for(const auto& track : tracks) {
        if(track.isInLibrary() && track.isEnabled()) {
            titles.push_back(track.title());
        }
    }

    std::ranges::sort(titles);
    return titles;
}

QVariantList waitForSignal(QSignalSpy& spy, int timeoutMs = 10000)
{
    if(spy.isEmpty()) {
        EXPECT_TRUE(spy.wait(timeoutMs));
    }

    EXPECT_FALSE(spy.isEmpty());
    return spy.isEmpty() ? QVariantList{} : spy.takeFirst();
}

void expectFinishedSignal(const QVariantList& args, int expectedId, Fooyin::ScanRequest::Type expectedType)
{
    ASSERT_EQ(args.size(), 3);
    EXPECT_EQ(args.at(0).toInt(), expectedId);
    EXPECT_EQ(args.at(1).value<Fooyin::ScanRequest::Type>(), expectedType);
    EXPECT_FALSE(args.at(2).toBool());
}

void expectSummarySignal(const QVariantList& args, int expectedId, Fooyin::ScanRequest::Type expectedType,
                         const Fooyin::ScanSummaryCounts& expectedSummary)
{
    ASSERT_EQ(args.size(), 3);
    EXPECT_EQ(args.at(0).toInt(), expectedId);
    EXPECT_EQ(args.at(1).value<Fooyin::ScanRequest::Type>(), expectedType);

    const auto summary = args.at(2).value<Fooyin::ScanSummaryCounts>();
    EXPECT_EQ(summary.added, expectedSummary.added);
    EXPECT_EQ(summary.updated, expectedSummary.updated);
    EXPECT_EQ(summary.removed, expectedSummary.removed);
}

class LibrarySortBarrier
{
public:
    LibrarySortBarrier()
        : m_previousMaxThreadCount{QThreadPool::globalInstance()->maxThreadCount()}
    {
        QThreadPool::globalInstance()->waitForDone();
        QThreadPool::globalInstance()->setMaxThreadCount(1);
        std::promise<void> entered;
        const auto ready = entered.get_future();
        m_blocker        = QtConcurrent::run([this, &entered]() {
            entered.set_value();
            std::unique_lock lock{m_mutex};
            m_ready.wait(lock, [this]() { return m_released; });
        });
        ready.wait();
    }
    ~LibrarySortBarrier()
    {
        release();
        m_blocker.waitForFinished();
        QThreadPool::globalInstance()->setMaxThreadCount(m_previousMaxThreadCount);
    }
    void release()
    {
        {
            const std::scoped_lock lock{m_mutex};
            m_released = true;
        }
        m_ready.notify_all();
    }

private:
    int m_previousMaxThreadCount;
    std::mutex m_mutex;
    std::condition_variable m_ready;
    bool m_released{false};
    QFuture<void> m_blocker;
};

class FakeLibraryReader : public Fooyin::AudioReader
{
public:
    struct State
    {
        void setTitle(const QString& path, const QString& title)
        {
            const std::scoped_lock lock(m_mutex);
            m_titles.insert_or_assign(QFileInfo{path}.absoluteFilePath(), title);
        }

        [[nodiscard]] QString titleForPath(const QString& path) const
        {
            const std::scoped_lock lock(m_mutex);
            auto title = m_titles.find(QFileInfo{path}.absoluteFilePath());
            if(title == m_titles.cend()) {
                return QFileInfo{path}.completeBaseName();
            }
            return title->second;
        }

        void setExtraTags(const QString& path, std::map<QString, QStringList> tags)
        {
            const std::scoped_lock lock(m_mutex);
            m_extraTags.insert_or_assign(QFileInfo{path}.absoluteFilePath(), std::move(tags));
        }

        [[nodiscard]] std::map<QString, QStringList> extraTagsForPath(const QString& path) const
        {
            const std::scoped_lock lock(m_mutex);
            const auto tags = m_extraTags.find(QFileInfo{path}.absoluteFilePath());
            return tags != m_extraTags.cend() ? tags->second : std::map<QString, QStringList>{};
        }

        void addWrite(const Fooyin::Track& track, WriteOptions options)
        {
            const std::scoped_lock lock(m_mutex);
            m_writes.emplace_back(track, options);

            if(options.testFlag(Rating)) {
                const auto policy = Fooyin::ratingTagPolicy();
                const auto tag    = policy.effectiveWriteTag();
                if(!tag.isEmpty()) {
                    auto& tags          = m_extraTags[QFileInfo{track.filepath()}.absoluteFilePath()];
                    const QString value = Fooyin::formatTextRating(track.rating(), policy.writeScale);
                    if(value.isEmpty()) {
                        tags.erase(tag);
                    }
                    else {
                        tags.insert_or_assign(tag, QStringList{value});
                    }
                }
            }
        }

        [[nodiscard]] std::vector<std::pair<Fooyin::Track, WriteOptions>> writes() const
        {
            const std::scoped_lock lock(m_mutex);
            return m_writes;
        }

    private:
        mutable std::mutex m_mutex;
        std::unordered_map<QString, QString> m_titles;
        std::map<QString, std::map<QString, QStringList>> m_extraTags;
        std::vector<std::pair<Fooyin::Track, WriteOptions>> m_writes;
    };

    explicit FakeLibraryReader(std::shared_ptr<State> state)
        : m_state{std::move(state)}
    { }

    QStringList extensions() const override
    {
        return {u"mp3"_s};
    }

    bool canReadCover() const override
    {
        return false;
    }

    bool canWriteMetaData() const override
    {
        return true;
    }

    bool readTrack(const Fooyin::AudioSource& source, Fooyin::Track& track) override
    {
        const QFileInfo info{source.filepath};
        track.setTitle(m_state->titleForPath(info.absoluteFilePath()));
        track.setAlbum(u"Library Test Album"_s);
        track.setArtists({u"Library Test Artist"_s});
        track.setAlbumArtists({u"Library Test Artist"_s});

        const auto match = QRegularExpression{u"(\\d+)"_s}.match(info.completeBaseName());
        track.setTrackNumber(match.hasMatch() ? match.captured(1) : u"1"_s);
        track.setDiscNumber(u"1"_s);

        const auto extraTags = m_state->extraTagsForPath(info.absoluteFilePath());
        for(const auto& [field, values] : extraTags) {
            track.addExtraTag(field, values);
        }

        return true;
    }

    bool writeTrack(const Fooyin::AudioSource& /*source*/, const Fooyin::Track& track, WriteOptions options) override
    {
        m_state->addWrite(track, options);
        return true;
    }

private:
    std::shared_ptr<State> m_state;
};

struct FakeArchiveReaderState
{
    std::map<QString, QByteArray> entries;
};

class FakeArchiveReader : public Fooyin::ArchiveReader
{
public:
    explicit FakeArchiveReader(std::shared_ptr<FakeArchiveReaderState> state)
        : m_state{std::move(state)}
    { }

    QStringList extensions() const override
    {
        return {u"zip"_s};
    }

    QString type() const override
    {
        return u"zip"_s;
    }

    bool init(const QString& /*file*/) override
    {
        return true;
    }

    Fooyin::ArchiveEntryData entry(const QString& file) override
    {
        const auto entry = m_state->entries.find(file);
        if(entry == m_state->entries.cend()) {
            return {};
        }

        auto buffer = std::make_unique<QBuffer>();
        buffer->setData(entry->second);
        buffer->open(QIODevice::ReadOnly);

        return {
            .info = {.path = entry->first, .size = static_cast<uint64_t>(entry->second.size()), .isRegularFile = true},
            .device = std::move(buffer),
        };
    }

    bool readTracks(ReadEntryCallback readEntry, const StopRequestedCallback& stopRequested) override
    {
        for(const auto& path : m_state->entries | std::views::keys) {
            if(stopRequested && stopRequested()) {
                return false;
            }
            readEntry(entry(path));
        }
        return true;
    }

    QByteArray readCover(const Fooyin::Track& /*track*/, Fooyin::Track::Cover /*cover*/) override
    {
        return {};
    }

private:
    std::shared_ptr<FakeArchiveReaderState> m_state;
};

struct LibraryTestContext
{
    LibraryTestContext()
        : settings{tempDir.filePath(u"settings.ini"_s)}
        , coreSettings{&settings}
        , dbPool{createTestDbPool(tempDir.filePath(u"library.db"_s))}
        , dbHandler{dbPool}
        , libraryManager{dbPool, &settings}
        , playlistLoader{std::make_shared<Fooyin::PlaylistLoader>()}
        , audioLoader{std::make_shared<Fooyin::AudioLoader>()}
        , readerState{std::make_shared<FakeLibraryReader::State>()}
        , archiveReaderState{std::make_shared<FakeArchiveReaderState>()}
        , network{std::make_shared<Fooyin::NetworkAccessManager>(&settings)}
        , remoteIo{std::make_shared<Fooyin::RemoteIoService>(network, &settings)}
        , library{&libraryManager, dbPool, playlistLoader, audioLoader, remoteIo, &settings}
        , playlistHandler{dbPool, audioLoader, &library, &settings}
    {
        audioLoader->addReader(u"fake-library-reader"_s,
                               [state = readerState]() { return std::make_unique<FakeLibraryReader>(state); });
        audioLoader->addArchiveReader(u"fake-archive-reader"_s, [state = archiveReaderState]() {
            return std::make_unique<FakeArchiveReader>(state);
        });
    }

    QTemporaryDir tempDir;
    Fooyin::SettingsManager settings;
    Fooyin::CoreSettings coreSettings;
    Fooyin::DbConnectionPoolPtr dbPool;
    Fooyin::DbConnectionHandler dbHandler;
    Fooyin::LibraryManager libraryManager;
    std::shared_ptr<Fooyin::PlaylistLoader> playlistLoader;
    std::shared_ptr<Fooyin::AudioLoader> audioLoader;
    std::shared_ptr<FakeLibraryReader::State> readerState;
    std::shared_ptr<FakeArchiveReaderState> archiveReaderState;
    std::shared_ptr<Fooyin::NetworkAccessManager> network;
    std::shared_ptr<Fooyin::RemoteIoService> remoteIo;
    Fooyin::UnifiedMusicLibrary library;
    Fooyin::PlaylistHandler playlistHandler;
};
} // namespace

namespace Fooyin::Testing {
class UnifiedMusicLibraryTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        initialiseTestEnvironment();
        resetRatingSettings();

        m_context = std::make_unique<LibraryTestContext>();
        ASSERT_TRUE(m_context->tempDir.isValid());
        requireWorkingDatabase(m_context->dbPool);
    }

    [[nodiscard]] LibraryTestContext& context() const
    {
        return *m_context;
    }

    QString createTrackFile(const QString& fileName, const QString& title)
    {
        const QString path = context().tempDir.filePath(fileName);
        writeFile(path);
        context().readerState->setTitle(path, title);
        return path;
    }

    LibraryInfo addLibrary(const QString& name)
    {
        const int libraryId = insertLibrary(context().dbPool, context().tempDir.path(), name);
        EXPECT_GT(libraryId, 0);

        context().libraryManager.reset();

        const auto libraryInfo = context().libraryManager.libraryInfo(libraryId);
        EXPECT_TRUE(libraryInfo.has_value());
        return libraryInfo.value_or(LibraryInfo{});
    }

    void waitForSuccessfulScan(const std::function<ScanRequest()>& startScan,
                               ScanRequest::Type type = ScanRequest::Library)
    {
        QSignalSpy finishedSpy{&context().library, &MusicLibrary::scanFinished};

        const ScanRequest request = startScan();
        expectFinishedSignal(waitForSignal(finishedSpy), request.id, type);
    }

private:
    std::unique_ptr<LibraryTestContext> m_context;
};

TEST_F(UnifiedMusicLibraryTest, ScanForChangesMakesTracksVisibleBeforeScanFinished)
{
    createTrackFile(u"initial_track.mp3"_s, u"Initial"_s);

    const LibraryInfo libraryInfo = addLibrary(u"Refresh Visibility"_s);
    ASSERT_GE(libraryInfo.id, 0);

    waitForSuccessfulScan([&]() { return context().library.rescan(libraryInfo); });
    ASSERT_EQ(context().library.tracks().size(), 1);

    createTrackFile(u"new_track.mp3"_s, u"Added Later"_s);

    QSignalSpy summarySpy{&context().library, &MusicLibrary::scanSummary};
    QSignalSpy finishedSpy{&context().library, &MusicLibrary::scanFinished};

    const ScanRequest request       = context().library.refresh(libraryInfo);
    const QVariantList finishedArgs = waitForSignal(finishedSpy);
    const int tracksAtFinish        = static_cast<int>(context().library.tracks().size());
    const QVariantList summaryArgs  = waitForSignal(summarySpy);

    expectFinishedSignal(finishedArgs, request.id, ScanRequest::Library);
    expectSummarySignal(summaryArgs, request.id, ScanRequest::Library, {.added = 1, .updated = 0, .removed = 0});

    EXPECT_EQ(tracksAtFinish, 2);
    EXPECT_EQ(context().library.tracks().size(), 2);
    EXPECT_EQ(sortedTrackTitles(context().library.tracks()), (QStringList{u"Added Later"_s, u"Initial"_s}));
}

TEST_F(UnifiedMusicLibraryTest, ExcludingArchiveTypeRemovesPreviouslyIndexedEntries)
{
    const QString archivePath = context().tempDir.filePath(u"album.zip"_s);
    writeFile(archivePath, "archive");
    context().archiveReaderState->entries.emplace(u"track.mp3"_s, "track");

    const LibraryInfo libraryInfo = addLibrary(u"Archive Exclusion"_s);
    ASSERT_GE(libraryInfo.id, 0);

    waitForSuccessfulScan([&]() { return context().library.rescan(libraryInfo); });
    ASSERT_EQ(context().library.tracks().size(), 1);
    EXPECT_TRUE(context().library.tracks().front().isInArchive());

    context().settings.fileSet(Settings::Core::Internal::LibraryExcludeTypes, QStringList{u"zip"_s});

    QSignalSpy summarySpy{&context().library, &MusicLibrary::scanSummary};
    QSignalSpy finishedSpy{&context().library, &MusicLibrary::scanFinished};

    const ScanRequest request       = context().library.rescan(libraryInfo);
    const QVariantList finishedArgs = waitForSignal(finishedSpy);
    const QVariantList summaryArgs  = waitForSignal(summarySpy);

    expectFinishedSignal(finishedArgs, request.id, ScanRequest::Library);
    expectSummarySignal(summaryArgs, request.id, ScanRequest::Library, {.added = 0, .updated = 0, .removed = 1});
    ASSERT_EQ(context().library.tracks().size(), 1);
    EXPECT_FALSE(context().library.tracks().front().isInLibrary());
    EXPECT_FALSE(context().library.tracks().front().isEnabled());
    EXPECT_TRUE(QFileInfo::exists(archivePath));
}

TEST_F(UnifiedMusicLibraryTest, RemovingCueEntryRemovesOnlyStaleTrack)
{
    createTrackFile(u"album.mp3"_s, u"Backing File"_s);
    const QString cuePath = context().tempDir.filePath(u"album.cue"_s);
    writeFile(cuePath, "FILE \"album.mp3\" MP3\n"
                       "  TRACK 01 AUDIO\n"
                       "    TITLE \"Cue One\"\n"
                       "    INDEX 01 00:00:00\n"
                       "  TRACK 02 AUDIO\n"
                       "    TITLE \"Cue Two\"\n"
                       "    INDEX 01 01:00:00\n");
    context().playlistLoader->addParser(std::make_unique<CueParser>());
    context().settings.fileSet(Settings::Core::Internal::LibraryExcludeTypes, QStringList{});

    const LibraryInfo libraryInfo = addLibrary(u"Changed CUE"_s);
    ASSERT_GE(libraryInfo.id, 0);

    waitForSuccessfulScan([&]() { return context().library.rescan(libraryInfo); });
    ASSERT_EQ(activeTrackTitles(context().library.tracks()), (QStringList{u"Cue One"_s, u"Cue Two"_s}));

    writeFile(cuePath, "FILE \"album.mp3\" MP3\n"
                       "  TRACK 01 AUDIO\n"
                       "    TITLE \"Cue One\"\n"
                       "    INDEX 01 00:00:00\n");

    QSignalSpy summarySpy{&context().library, &MusicLibrary::scanSummary};
    QSignalSpy finishedSpy{&context().library, &MusicLibrary::scanFinished};

    const ScanRequest request       = context().library.rescan(libraryInfo);
    const QVariantList finishedArgs = waitForSignal(finishedSpy);
    const QVariantList summaryArgs  = waitForSignal(summarySpy);

    expectFinishedSignal(finishedArgs, request.id, ScanRequest::Library);
    expectSummarySignal(summaryArgs, request.id, ScanRequest::Library, {.added = 0, .updated = 1, .removed = 1});
    EXPECT_EQ(activeTrackTitles(context().library.tracks()), (QStringList{u"Cue One"_s}));
}

TEST_F(UnifiedMusicLibraryTest, MultipleScanUpdatesCommitInOrder)
{
    constexpr int TrackCount = 300;

    for(int i = 0; i < TrackCount; ++i) {
        const QString fileName = u"track_%1.mp3"_s.arg(i, 3, 10, QChar{u'0'});
        createTrackFile(fileName, u"Track %1"_s.arg(i));
    }

    const LibraryInfo libraryInfo = addLibrary(u"Batch Commit Order"_s);
    ASSERT_GE(libraryInfo.id, 0);

    QSignalSpy applySpy{&context().library, &MusicLibrary::scanApplyCompleted};
    QSignalSpy summarySpy{&context().library, &MusicLibrary::scanSummary};
    QSignalSpy finishedSpy{&context().library, &MusicLibrary::scanFinished};

    const ScanRequest request = context().library.rescan(libraryInfo);
    ASSERT_TRUE(waitForCondition([&]() { return applySpy.count() >= 2; }));

    const QVariantList finishedArgs = waitForSignal(finishedSpy);
    const int tracksAtFinish        = static_cast<int>(context().library.tracks().size());
    const QVariantList summaryArgs  = waitForSignal(summarySpy);

    for(int i{0}; i < applySpy.count(); ++i) {
        const QVariantList& applyArgs = applySpy.at(i);
        ASSERT_EQ(applyArgs.size(), 1);
        EXPECT_EQ(applyArgs.at(0).toInt(), request.id);
    }

    expectFinishedSignal(finishedArgs, request.id, ScanRequest::Library);
    expectSummarySignal(summaryArgs, request.id, ScanRequest::Library,
                        {.added = TrackCount, .updated = 0, .removed = 0});

    EXPECT_EQ(summarySpy.count(), 0);
    EXPECT_EQ(finishedSpy.count(), 0);
    EXPECT_EQ(tracksAtFinish, TrackCount);
    EXPECT_EQ(static_cast<int>(context().library.tracks().size()), TrackCount);
    EXPECT_EQ(applySpy.count(), 2);
}

TEST_F(UnifiedMusicLibraryTest, TrackRescanUpdatesMetadataBeforeScanFinished)
{
    const QString filePath = createTrackFile(u"single_track.mp3"_s, u"Before"_s);

    const LibraryInfo libraryInfo = addLibrary(u"Rescan Metadata"_s);
    ASSERT_GE(libraryInfo.id, 0);

    waitForSuccessfulScan([&]() { return context().library.rescan(libraryInfo); });
    ASSERT_EQ(context().library.tracks().size(), 1);

    const Track existingTrack = context().library.tracks().front();
    ASSERT_TRUE(existingTrack.isValid());
    ASSERT_EQ(existingTrack.title(), u"Before"_s);

    context().readerState->setTitle(filePath, u"After"_s);

    QSignalSpy summarySpy{&context().library, &MusicLibrary::scanSummary};
    QSignalSpy finishedSpy{&context().library, &MusicLibrary::scanFinished};

    const ScanRequest request       = context().library.scanTracks({existingTrack});
    const QVariantList finishedArgs = waitForSignal(finishedSpy);
    const QString titleAtFinish     = context().library.trackForId(existingTrack.id()).title();
    const QVariantList summaryArgs  = waitForSignal(summarySpy);

    expectFinishedSignal(finishedArgs, request.id, ScanRequest::Tracks);
    expectSummarySignal(summaryArgs, request.id, ScanRequest::Tracks, {.added = 0, .updated = 1, .removed = 0});

    EXPECT_EQ(titleAtFinish, u"After"_s);
    EXPECT_EQ(context().library.trackForId(existingTrack.id()).title(), u"After"_s);
}

TEST_F(UnifiedMusicLibraryTest, TrackRescanReloadsEmbeddedCueTracks)
{
    const QString filePath = createTrackFile(u"embedded.mp3"_s, u"Backing File"_s);
    const QString cueSheet = uR"(FILE "embedded.mp3" MP3
  TRACK 01 AUDIO
    TITLE "Cue One"
    INDEX 01 00:00:00
  TRACK 02 AUDIO
    TITLE "Cue Two"
    INDEX 01 01:00:00
)"_s;

    context().readerState->setExtraTags(filePath, {{u"CUESHEET"_s, {cueSheet}},
                                                   {u"CUE_TRACK01_CONDUCTOR"_s, {u"First Conductor"_s}},
                                                   {u"CUE_TRACK02_CONDUCTOR"_s, {u"Second Conductor"_s}}});
    context().playlistLoader->addParser(std::make_unique<CueParser>());

    const LibraryInfo libraryInfo = addLibrary(u"Embedded CUE reload"_s);
    ASSERT_GE(libraryInfo.id, 0);
    waitForSuccessfulScan([&]() { return context().library.rescan(libraryInfo); });

    TrackList cueTracks = context().library.tracks();
    ASSERT_EQ(2, cueTracks.size());
    ASSERT_TRUE(std::ranges::all_of(cueTracks, &Track::hasEmbeddedCue));

    std::map<QString, int> idsByTrackNumber;
    for(const Track& track : cueTracks) {
        idsByTrackNumber.emplace(track.trackNumber(), track.id());
    }

    const QString updatedCueSheet = QString{cueSheet}.replace(u"Cue Two"_s, u"Cue Two Reloaded"_s);
    context().readerState->setExtraTags(filePath, {{u"CUESHEET"_s, {updatedCueSheet}},
                                                   {u"CUE_TRACK01_CONDUCTOR"_s, {u"First Reloaded"_s}},
                                                   {u"CUE_TRACK02_CONDUCTOR"_s, {u"Second Reloaded"_s}}});

    waitForSuccessfulScan([&]() { return context().library.scanTracks(cueTracks); }, ScanRequest::Tracks);

    cueTracks = context().library.tracks();
    ASSERT_EQ(2, cueTracks.size());
    for(const Track& track : cueTracks) {
        EXPECT_EQ(idsByTrackNumber.at(track.trackNumber()), track.id());
        EXPECT_EQ(QStringList{track.trackNumber() == u"01"_s ? u"First Reloaded"_s : u"Second Reloaded"_s},
                  track.extraTag(u"CONDUCTOR"_s));
        EXPECT_FALSE(std::ranges::any_of(track.extraTags(), [](const auto& tag) {
            return tag.first.startsWith(u"CUE_TRACK"_s, Qt::CaseInsensitive);
        }));
    }
    EXPECT_EQ(activeTrackTitles(cueTracks), (QStringList{u"Cue One"_s, u"Cue Two Reloaded"_s}));
}

TEST_F(UnifiedMusicLibraryTest, TrackRescanReloadsExternalCueTracks)
{
    createTrackFile(u"external.mp3"_s, u"Backing File"_s);
    const QString cuePath = context().tempDir.filePath(u"external.cue"_s);
    writeFile(cuePath, "FILE \"external.mp3\" MP3\n"
                       "  TRACK 01 AUDIO\n"
                       "    TITLE \"Cue One\"\n"
                       "    INDEX 01 00:00:00\n"
                       "  TRACK 02 AUDIO\n"
                       "    TITLE \"Cue Two\"\n"
                       "    INDEX 01 01:00:00\n");
    context().playlistLoader->addParser(std::make_unique<CueParser>());
    context().settings.fileSet(Settings::Core::Internal::LibraryExcludeTypes, QStringList{});

    const LibraryInfo libraryInfo = addLibrary(u"External CUE reload"_s);
    ASSERT_GE(libraryInfo.id, 0);
    waitForSuccessfulScan([&]() { return context().library.rescan(libraryInfo); });

    TrackList cueTracks = context().library.tracks();
    ASSERT_EQ(2, cueTracks.size());
    ASSERT_TRUE(
        std::ranges::all_of(cueTracks, [](const Track& track) { return track.hasCue() && !track.hasEmbeddedCue(); }));

    std::map<QString, int> idsByTrackNumber;
    for(const Track& track : cueTracks) {
        idsByTrackNumber.emplace(track.trackNumber(), track.id());
    }

    writeFile(cuePath, "FILE \"external.mp3\" MP3\n"
                       "  TRACK 01 AUDIO\n"
                       "    TITLE \"Cue One Reloaded\"\n"
                       "    INDEX 01 00:00:00\n"
                       "  TRACK 02 AUDIO\n"
                       "    TITLE \"Cue Two Reloaded\"\n"
                       "    INDEX 01 01:00:00\n");

    waitForSuccessfulScan([&]() { return context().library.scanTracks(cueTracks); }, ScanRequest::Tracks);

    cueTracks = context().library.tracks();
    ASSERT_EQ(2, cueTracks.size());
    for(const Track& track : cueTracks) {
        EXPECT_EQ(idsByTrackNumber.at(track.trackNumber()), track.id());
    }
    EXPECT_EQ(activeTrackTitles(cueTracks), (QStringList{u"Cue One Reloaded"_s, u"Cue Two Reloaded"_s}));
}

TEST_F(UnifiedMusicLibraryTest, DeferredWritesMergeMetadataAndStatsSnapshots)
{
    ASSERT_TRUE(context().settings.set<Settings::Core::SaveRatingToMetadata>(true));

    createTrackFile(u"deferred_write.mp3"_s, u"Before"_s);

    const LibraryInfo libraryInfo = addLibrary(u"Deferred Writes"_s);
    ASSERT_GE(libraryInfo.id, 0);

    waitForSuccessfulScan([&]() { return context().library.rescan(libraryInfo); });
    ASSERT_EQ(context().library.tracks().size(), 1);

    const Track originalTrack = context().library.tracks().front();
    context().library.setActivePlaybackTrack(originalTrack);

    Track statsTrack{originalTrack};
    statsTrack.setRating(0.8F);
    context().library.updateTrackStats(statsTrack, Track::Stat::Rating);

    ASSERT_TRUE(waitForCondition([&]() { return context().library.trackForId(originalTrack.id()).rating() == 0.8F; }));

    Track metadataTrack{originalTrack};
    metadataTrack.setTitle(u"After"_s);
    context().library.writeTrackMetadata({metadataTrack});
    context().library.flushPendingWrites();

    ASSERT_TRUE(waitForCondition([&]() { return context().readerState->writes().size() == 2; }));

    const auto writes = context().readerState->writes();
    ASSERT_EQ(writes.size(), 2);
    EXPECT_EQ(writes.at(0).first.title(), u"After"_s);
    EXPECT_FLOAT_EQ(writes.at(0).first.rating(), 0.8F);
    EXPECT_TRUE(writes.at(0).second.testFlag(AudioReader::Metadata));
    EXPECT_EQ(writes.at(1).first.title(), u"After"_s);
    EXPECT_FLOAT_EQ(writes.at(1).first.rating(), 0.8F);
    EXPECT_EQ(writes.at(1).second, AudioReader::Rating);
}

TEST_F(UnifiedMusicLibraryTest, VirtualTrackMetadataIsStoredOnlyInDatabase)
{
    Track virtualTrack{u"cdda:///I5l9cCSFccLKFEKS.7wqSZAorPU-"_s, 0};
    virtualTrack.setTitle(u"Before"_s);

    EXPECT_FALSE(virtualTrack.isMetadataEditable(context().audioLoader->canWriteMetadata(virtualTrack)));

    auto* playlist = context().playlistHandler.createPlaylist(u"Audio CD"_s, {virtualTrack});
    ASSERT_NE(playlist, nullptr);
    ASSERT_EQ(playlist->trackCount(), 1);

    virtualTrack = playlist->tracks().front();
    ASSERT_TRUE(virtualTrack.isInDatabase());
    ASSERT_TRUE(virtualTrack.isMetadataEditable(context().audioLoader->canWriteMetadata(virtualTrack)));

    virtualTrack.setTitle(u"After"_s);
    QSignalSpy metadataSpy{&context().library, &MusicLibrary::tracksMetadataChanged};
    context().library.writeTrackMetadata({virtualTrack});
    waitForSignal(metadataSpy);

    EXPECT_TRUE(context().readerState->writes().empty());

    QSignalSpy loadedSpy{&context().library, &MusicLibrary::tracksLoaded};
    context().library.loadAllTracks();
    waitForSignal(loadedSpy);

    EXPECT_EQ(context().library.trackForId(virtualTrack.id()).title(), u"After"_s);
}

TEST_F(UnifiedMusicLibraryTest, StalePlaycountUpdatePreservesPendingRating)
{
    ASSERT_TRUE(context().settings.set<Settings::Core::SaveRatingToMetadata>(true));
    ASSERT_TRUE(context().settings.set<Settings::Core::SavePlaycountToMetadata>(true));

    createTrackFile(u"stale_playcount.mp3"_s, u"Track"_s);

    const LibraryInfo libraryInfo = addLibrary(u"Stale Playcount"_s);
    ASSERT_GE(libraryInfo.id, 0);

    waitForSuccessfulScan([&]() { return context().library.rescan(libraryInfo); });
    ASSERT_EQ(context().library.tracks().size(), 1);

    const Track originalTrack = context().library.tracks().front();
    context().library.setActivePlaybackTrack(originalTrack);

    Track ratingTrack{originalTrack};
    ratingTrack.setRating(0.4F);
    context().library.updateTrackStats(ratingTrack, Track::Stat::Rating);
    ASSERT_TRUE(waitForCondition([&]() { return context().library.trackForId(originalTrack.id()).rating() == 0.4F; }));

    const Track stalePlaycountTrack{originalTrack};
    QSignalSpy staleUpdateSpy{&context().library, &MusicLibrary::tracksUpdated};
    context().library.updateTracks({stalePlaycountTrack});
    waitForSignal(staleUpdateSpy);
    ASSERT_FLOAT_EQ(context().library.trackForId(originalTrack.id()).rating(), originalTrack.rating());

    QSignalSpy updateSpy{&context().library, &MusicLibrary::tracksUpdated};
    context().library.trackWasPlayed(stalePlaycountTrack);

    const QVariantList updateArgs = waitForSignal(updateSpy);
    ASSERT_EQ(updateArgs.size(), 1);
    const auto updatedTracks = updateArgs.front().value<TrackList>();
    ASSERT_EQ(updatedTracks.size(), 1);
    EXPECT_EQ(updatedTracks.front().playCount(), 1);
    EXPECT_FLOAT_EQ(updatedTracks.front().rating(), 0.4F);

    context().library.flushPendingWrites();
    ASSERT_TRUE(waitForCondition([&]() { return context().readerState->writes().size() == 1; }));

    const auto writes = context().readerState->writes();
    ASSERT_EQ(writes.size(), 1);
    EXPECT_FLOAT_EQ(writes.front().first.rating(), 0.4F);
    EXPECT_EQ(writes.front().first.playCount(), 1);
    EXPECT_EQ(writes.front().second, AudioReader::Rating | AudioReader::Playcount);
}

TEST_F(UnifiedMusicLibraryTest, RatingUpdateDoesNotWriteMetadataWhenDisabled)
{
    context().settings.set<Settings::Core::SaveRatingToMetadata>(false);
    ASSERT_FALSE(context().settings.value<Settings::Core::SaveRatingToMetadata>());

    createTrackFile(u"rating_update.mp3"_s, u"Track"_s);

    const LibraryInfo libraryInfo = addLibrary(u"Rating Update"_s);
    ASSERT_GE(libraryInfo.id, 0);

    waitForSuccessfulScan([&]() { return context().library.rescan(libraryInfo); });
    ASSERT_EQ(context().library.tracks().size(), 1);

    Track track = context().library.tracks().front();
    track.setRating(0.8F);
    context().library.updateTrackStats(track, Track::Stat::Rating);

    ASSERT_TRUE(waitForCondition([&]() { return context().library.trackForId(track.id()).rating() == 0.8F; }));
    EXPECT_TRUE(context().readerState->writes().empty());
}

TEST_F(UnifiedMusicLibraryTest, RatingWriteUpdatesRawRatingTag)
{
    ASSERT_TRUE(context().settings.set<Settings::Core::SaveRatingToMetadata>(true));

    {
        FySettings settings;
        settings.setValue(RatingSettings::WriteTag, u"RATING"_s);
        settings.setValue(RatingSettings::WriteScale, u"OneToFive"_s);
    }

    createTrackFile(u"raw_rating.mp3"_s, u"Track"_s);

    const LibraryInfo libraryInfo = addLibrary(u"Raw Rating"_s);
    ASSERT_GE(libraryInfo.id, 0);

    waitForSuccessfulScan([&]() { return context().library.rescan(libraryInfo); });
    ASSERT_EQ(context().library.tracks().size(), 1);

    Track track = context().library.tracks().front();
    track.setRating(0.6F);
    track.setRawRatingTag(u"RATING"_s, u"3"_s);

    QSignalSpy metadataSpy{&context().library, &MusicLibrary::tracksMetadataChanged};
    context().library.updateTrackMetadata({track});
    waitForSignal(metadataSpy);

    track = context().library.trackForId(track.id());
    ASSERT_EQ(track.rawRatingTag(u"RATING"_s), u"3"_s);

    track.setRating(1.0F);
    context().library.updateTrackStats(track, Track::Stat::Rating);

    ASSERT_TRUE(waitForCondition([&]() {
        const Track updatedTrack = context().library.trackForId(track.id());
        return updatedTrack.rating() == 1.0F && updatedTrack.rawRatingTag(u"RATING"_s) == u"5"_s;
    }));

    const auto writes = context().readerState->writes();
    ASSERT_EQ(writes.size(), 1);
    EXPECT_EQ(writes.front().second, AudioReader::Rating);

    QSignalSpy loadedSpy{&context().library, &MusicLibrary::tracksLoaded};
    context().library.loadAllTracks();
    waitForSignal(loadedSpy);

    const Track restoredTrack = context().library.trackForId(track.id());
    EXPECT_FLOAT_EQ(restoredTrack.rating(), 1.0F);
    EXPECT_EQ(restoredTrack.rawRatingTag(u"RATING"_s), u"5"_s);
}

TEST_F(UnifiedMusicLibraryTest, UnmanagedRatingWriteUpdatesPlaylistRawRatingTag)
{
    ASSERT_TRUE(context().settings.set<Settings::Core::SaveRatingToMetadata>(true));

    {
        FySettings settings;
        settings.setValue(RatingSettings::WriteTag, u"RATING"_s);
        settings.setValue(RatingSettings::WriteScale, u"OneToFive"_s);
    }

    QTemporaryDir externalDir;
    ASSERT_TRUE(externalDir.isValid());
    const QString path = externalDir.filePath(u"raw_rating.mp3"_s);
    writeFile(path);
    context().readerState->setTitle(path, u"Track"_s);

    QSignalSpy scannedSpy{&context().library, &MusicLibrary::tracksScanned};
    waitForSuccessfulScan([&]() { return context().library.scanFiles({QUrl::fromLocalFile(path)}); },
                          ScanRequest::Files);

    ASSERT_FALSE(scannedSpy.isEmpty());
    auto tracks = scannedSpy.takeLast().at(1).value<TrackList>();
    ASSERT_EQ(tracks.size(), 1);

    Track track = tracks.front();
    ASSERT_FALSE(track.isInLibrary());
    track.setRating(0.6F);
    track.setRawRatingTag(u"RATING"_s, u"3"_s);

    QSignalSpy metadataSpy{&context().library, &MusicLibrary::tracksMetadataChanged};
    context().library.updateTrackMetadata({track});
    waitForSignal(metadataSpy);

    track          = context().library.trackForId(track.id());
    auto* playlist = context().playlistHandler.createPlaylist(u"Unmanaged"_s, {track});
    ASSERT_NE(playlist, nullptr);

    track.setRating(1.0F);
    context().library.updateTrackStats(track, Track::Stat::Rating);

    ASSERT_TRUE(waitForCondition([&]() {
        if(playlist->tracks().empty()) {
            return false;
        }
        const Track& updatedTrack = playlist->tracks().front();
        return updatedTrack.rating() == 1.0F && updatedTrack.rawRatingTag(u"RATING"_s) == u"5"_s;
    }));
}

TEST_F(UnifiedMusicLibraryTest, RemoteFileMetadataUpdatesPlaylistAndDatabase)
{
    const QDateTime modifiedTime{QDate{2020, 11, 24}, QTime{10, 3, 40}, QTimeZone::utc()};
    TrackDatabase database;
    database.initialise(DbConnectionProvider{context().dbPool});
    database.setMetadataStore(context().library.metadataStore());
    TrackList tracks{Track{u"https://example.com/track.mp3"_s}};
    tracks.front().generateHash();

    ASSERT_TRUE(database.storeTracks(tracks));
    ASSERT_GE(tracks.front().id(), 0);
    auto* playlist = context().playlistHandler.createPlaylist(u"Remote file"_s, tracks);
    ASSERT_NE(playlist, nullptr);
    ASSERT_EQ(playlist->tracks().front().duration(), 0);

    Track updatedTrack{tracks.front()};
    updatedTrack.setDuration(398000);
    updatedTrack.setFileSize(6368861);
    updatedTrack.setModifiedTime(modifiedTime.toMSecsSinceEpoch());
    updatedTrack.setSampleRate(44100);
    updatedTrack.setCodec(u"MP3"_s);
    updatedTrack.setCodecProfile(u"CBR"_s);
    updatedTrack.setTool(u"LAME3.99r"_s);
    updatedTrack.setExtraProperty(u"ENC_DELAY"_s, u"576"_s);
    updatedTrack.setExtraProperty(u"ENC_PADDING"_s, u"648"_s);

    const QSignalSpy playlistSpy{&context().playlistHandler, &PlaylistHandler::tracksChanged};
    QSignalSpy metadataSpy{&context().library, &MusicLibrary::tracksMetadataChanged};
    context().library.updateTrackMetadata({updatedTrack});
    waitForSignal(metadataSpy);
    ASSERT_FALSE(playlistSpy.isEmpty());

    const Track playlistTrack = playlist->tracks().front();
    EXPECT_EQ(playlistTrack.duration(), 398000);
    EXPECT_EQ(playlistTrack.fileSize(), 6368861);
    EXPECT_EQ(playlistTrack.modifiedTime(), modifiedTime.toMSecsSinceEpoch());
    EXPECT_EQ(playlistTrack.codecProfile(), u"CBR"_s);
    EXPECT_EQ(playlistTrack.tool(), u"LAME3.99r"_s);
    EXPECT_EQ(playlistTrack.extraProperties().value(u"ENC_DELAY"_s), u"576"_s);
    EXPECT_EQ(playlistTrack.extraProperties().value(u"ENC_PADDING"_s), u"648"_s);

    Track restoredTrack{tracks.front()};
    ASSERT_TRUE(database.reloadTrack(restoredTrack));
    EXPECT_EQ(restoredTrack.duration(), playlistTrack.duration());
    EXPECT_EQ(restoredTrack.fileSize(), playlistTrack.fileSize());
    EXPECT_EQ(restoredTrack.modifiedTime(), playlistTrack.modifiedTime());
    EXPECT_EQ(restoredTrack.tool(), playlistTrack.tool());
    EXPECT_EQ(restoredTrack.extraProperties().value(u"ENC_DELAY"_s),
              playlistTrack.extraProperties().value(u"ENC_DELAY"_s));
    EXPECT_TRUE(context().readerState->writes().empty());
}

TEST_F(UnifiedMusicLibraryTest, OverlappingSortAndScanDoNotLoseNewTracks)
{
    ASSERT_TRUE(context().settings.set<Settings::Core::LibrarySortScript>(u"%title%"_s));

    createTrackFile(u"b.mp3"_s, u"Alpha"_s);
    createTrackFile(u"a.mp3"_s, u"Zulu"_s);

    const LibraryInfo libraryInfo = addLibrary(u"Sort Overlap"_s);
    ASSERT_GE(libraryInfo.id, 0);

    waitForSuccessfulScan([&]() { return context().library.rescan(libraryInfo); });
    ASSERT_EQ(trackFileNames(context().library.tracks()), (QStringList{u"b.mp3"_s, u"a.mp3"_s}));

    createTrackFile(u"c.mp3"_s, u"Beta"_s);

    QSignalSpy summarySpy{&context().library, &MusicLibrary::scanSummary};
    QSignalSpy finishedSpy{&context().library, &MusicLibrary::scanFinished};

    const ScanRequest request = context().library.refresh(libraryInfo);

    ASSERT_TRUE(context().settings.set<Settings::Core::LibrarySortScript>(u"%filepath%"_s));
    const QVariantList finishedArgs     = waitForSignal(finishedSpy);
    const QStringList fileNamesAtFinish = trackFileNames(context().library.tracks());
    const QVariantList summaryArgs      = waitForSignal(summarySpy);

    expectFinishedSignal(finishedArgs, request.id, ScanRequest::Library);
    expectSummarySignal(summaryArgs, request.id, ScanRequest::Library, {.added = 1, .updated = 0, .removed = 0});

    EXPECT_EQ(fileNamesAtFinish, (QStringList{u"a.mp3"_s, u"b.mp3"_s, u"c.mp3"_s}));
    EXPECT_EQ(sortedTrackTitles(context().library.tracks()), (QStringList{u"Alpha"_s, u"Beta"_s, u"Zulu"_s}));
}

TEST_F(UnifiedMusicLibraryTest, ReaddingLibraryUpdatesExistingPlaylistTracksAndEmitsMetadataChange)
{
    createTrackFile(u"restored_track.mp3"_s, u"Restored"_s);

    const LibraryInfo originalLibrary = addLibrary(u"Original Library"_s);
    ASSERT_GE(originalLibrary.id, 0);

    const ScanRequest initialRequest = context().library.rescan(originalLibrary);
    ASSERT_GE(initialRequest.id, 0);
    ASSERT_TRUE(waitForCondition([&]() { return context().library.tracks().size() == 1; }));
    ASSERT_EQ(context().library.tracks().size(), 1);

    const Track originalTrack = context().library.tracks().front();
    ASSERT_TRUE(originalTrack.isValid());
    ASSERT_TRUE(originalTrack.isInLibrary());

    auto* playlist = context().playlistHandler.createPlaylist(u"Regression Playlist"_s, context().library.tracks());
    ASSERT_NE(playlist, nullptr);
    ASSERT_EQ(playlist->trackCount(), 1);
    ASSERT_TRUE(playlist->tracks().front().isInLibrary());
    context().playlistHandler.savePlaylists();

    ASSERT_TRUE(context().libraryManager.removeLibrary(originalLibrary.id));
    ASSERT_TRUE(waitForCondition([&]() {
        const Track track = context().library.trackForId(originalTrack.id());
        return track.isValid() && !track.isInLibrary();
    }));

    context().playlistHandler.replacePlaylistTracks(playlist->id(), context().library.tracks());
    ASSERT_EQ(playlist->trackCount(), 1);
    ASSERT_FALSE(playlist->tracks().front().isInLibrary());

    const QSignalSpy metadataSpy{&context().library, &MusicLibrary::tracksMetadataChanged};

    const LibraryInfo readdedLibrary = addLibrary(u"Readded Library"_s);
    ASSERT_GE(readdedLibrary.id, 0);

    const ScanRequest request = context().library.rescan(readdedLibrary);
    ASSERT_GE(request.id, 0);

    const bool restored = waitForCondition([&]() {
        const Track track = context().library.trackForId(originalTrack.id());
        if(!track.isValid() || !track.isInLibrary()) {
            return false;
        }

        for(int i{0}; i < metadataSpy.count(); ++i) {
            const QVariantList& args = metadataSpy.at(i);
            if(args.size() != 1) {
                continue;
            }

            const auto tracks = args.front().value<TrackList>();
            if(std::ranges::any_of(tracks, [&](const Track& updatedTrack) {
                   return updatedTrack.id() == originalTrack.id() && updatedTrack.isInLibrary();
               })) {
                return true;
            }
        }

        return false;
    });

    EXPECT_TRUE(restored);

    bool sawRestoredTrack{false};
    for(int i{0}; i < metadataSpy.count(); ++i) {
        const QVariantList& args = metadataSpy.at(i);
        ASSERT_EQ(args.size(), 1);
        const auto tracks = args.front().value<TrackList>();
        if(std::ranges::any_of(
               tracks, [&](const Track& track) { return track.id() == originalTrack.id() && track.isInLibrary(); })) {
            sawRestoredTrack = true;
        }
    }

    EXPECT_TRUE(sawRestoredTrack);
    EXPECT_TRUE(context().library.trackForId(originalTrack.id()).isInLibrary());
    ASSERT_EQ(playlist->trackCount(), 1);
    EXPECT_TRUE(playlist->tracks().front().isInLibrary());
}

TEST_F(UnifiedMusicLibraryTest, RestoresStatsWhenUnmanagedTrackIsReadded)
{
    const QString path = createTrackFile(u"external.mp3"_s, u"External Track"_s);

    QSignalSpy scannedSpy{&context().library, &MusicLibrary::tracksScanned};
    ScanRequest firstRequest = context().library.scanFiles({QUrl::fromLocalFile(path)});
    waitForSuccessfulScan([&firstRequest]() { return firstRequest; }, ScanRequest::Files);

    ASSERT_FALSE(scannedSpy.isEmpty());
    auto tracks = scannedSpy.takeLast().at(1).value<TrackList>();
    ASSERT_EQ(tracks.size(), 1);

    const Track& track = tracks.front();
    ASSERT_GE(track.id(), 0);

    {
        const DbConnectionProvider dbProvider{context().dbPool};

        DbQuery query{
            dbProvider.db(),
            u"UPDATE TrackStats "
            "SET Loved = 1, PlayCount = 5, FirstPlayed = 100, LastPlayed = 500 "
            "WHERE TrackHash = :hash;"_s,
        };
        query.bindValue(u":hash"_s, track.hash());

        ASSERT_TRUE(query.exec()) << query.lastError().text().toStdString();
        ASSERT_EQ(query.numRowsAffected(), 1);
    }

    {
        const DbConnectionProvider dbProvider{context().dbPool};

        DbQuery query{
            dbProvider.db(),
            u"DELETE FROM Tracks WHERE TrackID = :id;"_s,
        };
        query.bindValue(u":id"_s, track.id());

        ASSERT_TRUE(query.exec()) << query.lastError().text().toStdString();
    }

    context().library.loadAllTracks();
    ASSERT_TRUE(waitForCondition([&]() { return context().library.tracks().empty(); }));

    scannedSpy.clear();

    ScanRequest secondRequest = context().library.scanFiles({QUrl::fromLocalFile(path)});
    waitForSuccessfulScan([&secondRequest]() { return secondRequest; }, ScanRequest::Files);

    ASSERT_FALSE(scannedSpy.isEmpty());
    tracks = scannedSpy.takeLast().at(1).value<TrackList>();

    ASSERT_EQ(tracks.size(), 1);
    EXPECT_TRUE(tracks.front().isLoved());
    EXPECT_EQ(tracks.front().playCount(), 5);
    EXPECT_EQ(tracks.front().firstPlayed(), 100);
    EXPECT_EQ(tracks.front().lastPlayed(), 500);
}
} // namespace Fooyin::Testing

namespace {
Fooyin::Track externalTrack(const QString& path, const QString& title)
{
    Fooyin::Track track{path};
    track.setTitle(title);
    track.setAlbum(u"External Album"_s);
    track.setArtists({u"External Artist"_s, u"Guest Artist"_s});
    track.setAlbumArtists({u"External Artist"_s});
    track.setDuration(180000);
    return track;
}

std::map<QString, Fooyin::Track> tracksByPath(const Fooyin::TrackList& tracks)
{
    std::map<QString, Fooyin::Track> result;
    for(const auto& track : tracks) {
        result.emplace(track.filepath(), track);
    }
    return result;
}

std::map<QString, QString> titlesByPath(const Fooyin::TrackList& tracks)
{
    std::map<QString, QString> result;
    for(const auto& track : tracks) {
        result.emplace(track.filepath(), track.title());
    }
    return result;
}

int trackRowCount(const Fooyin::DbConnectionPoolPtr& dbPool, const QString& condition, const QVariant& value)
{
    const Fooyin::DbConnectionProvider dbProvider{dbPool};

    Fooyin::DbQuery query{dbProvider.db(), u"SELECT COUNT(*) FROM Tracks WHERE %1;"_s.arg(condition)};
    query.bindValue(u":value"_s, value);
    EXPECT_TRUE(query.exec()) << query.lastError().text().toStdString();
    EXPECT_TRUE(query.next());

    return query.value(0).toInt();
}

void storeTrackStats(const Fooyin::DbConnectionPoolPtr& dbPool, const QString& hash, uint64_t added,
                     uint64_t firstPlayed, uint64_t lastPlayed, int playCount, float rating, bool loved)
{
    const Fooyin::DbConnectionProvider dbProvider{dbPool};

    Fooyin::DbQuery query{
        dbProvider.db(),
        u"INSERT OR REPLACE INTO TrackStats (TrackHash, AddedDate, FirstPlayed, LastPlayed, PlayCount, "
        "Rating, Loved) VALUES (:hash, :added, :firstPlayed, :lastPlayed, :playCount, :rating, :loved);"_s};
    query.bindValue(u":hash"_s, hash);
    query.bindValue(u":added"_s, QVariant::fromValue(added));
    query.bindValue(u":firstPlayed"_s, QVariant::fromValue(firstPlayed));
    query.bindValue(u":lastPlayed"_s, QVariant::fromValue(lastPlayed));
    query.bindValue(u":playCount"_s, playCount);
    query.bindValue(u":rating"_s, rating);
    query.bindValue(u":loved"_s, loved);
    EXPECT_TRUE(query.exec()) << query.lastError().text().toStdString();
}

void expectTrackStats(const Fooyin::Track& track, uint64_t added, uint64_t firstPlayed, uint64_t lastPlayed,
                      int playCount, float rating, bool loved)
{
    EXPECT_EQ(track.addedTime(), added) << track.filepath().toStdString();
    EXPECT_EQ(track.firstPlayed(), firstPlayed) << track.filepath().toStdString();
    EXPECT_EQ(track.lastPlayed(), lastPlayed) << track.filepath().toStdString();
    EXPECT_EQ(track.playCount(), playCount) << track.filepath().toStdString();
    EXPECT_FLOAT_EQ(track.rating(), rating) << track.filepath().toStdString();
    EXPECT_EQ(track.isLoved(), loved) << track.filepath().toStdString();
}

Fooyin::Track renamedTrack(const QString& path, const QString& title, const QString& artist, const QString& album)
{
    Fooyin::Track track{path};
    track.setTitle(title);
    track.setAlbum(album);
    track.setArtists({artist});
    track.setAlbumArtists({artist});
    track.setDuration(180000);
    return track;
}
} // namespace

namespace Fooyin::Testing {
TEST_F(UnifiedMusicLibraryTest, ExternalLibraryIsNotScannedAndRoutesRefreshToOwner)
{
    QSignalSpy finishedSpy{&context().library, &MusicLibrary::scanFinished};
    QSignalSpy refreshSpy{&context().library, &MusicLibrary::externalLibraryRefreshRequested};

    EXPECT_FALSE(context().library.addExternalLibrary(context().tempDir.path(), u"Not External"_s).has_value());
    EXPECT_FALSE(context().library.addExternalLibrary(u"test-source://catalog/"_s, u"Trailing"_s).has_value());

    const auto library = context().library.addExternalLibrary(u"test-source://catalog"_s, u"External"_s);
    ASSERT_TRUE(library.has_value());
    ASSERT_GT(library->id, 0);

    const auto sameLibrary = context().library.addExternalLibrary(u"test-source://catalog"_s, u"Again"_s);
    ASSERT_TRUE(sameLibrary.has_value());
    EXPECT_EQ(sameLibrary->id, library->id);

    const ScanRequest refreshRequest = context().library.refresh(*library);
    EXPECT_LT(refreshRequest.id, 0);
    const ScanRequest rescanRequest = context().library.rescan(*library);
    EXPECT_LT(rescanRequest.id, 0);
    context().library.refreshAll();

    ASSERT_TRUE(waitForCondition([&]() { return refreshSpy.size() >= 3; }));
    EXPECT_EQ(refreshSpy.front().front().value<LibraryInfo>().id, library->id);

    QCoreApplication::processEvents();
    EXPECT_TRUE(finishedSpy.isEmpty());
    EXPECT_TRUE(context().library.tracks().empty());
}

TEST_F(UnifiedMusicLibraryTest, ExternalSnapshotPreservesIdsAndDetachesRemovedTracks)
{
    const auto library = context().library.addExternalLibrary(u"test-source://catalog"_s, u"External"_s);
    ASSERT_TRUE(library.has_value());

    QSignalSpy appliedSpy{&context().library, &MusicLibrary::externalLibrarySnapshotFinished};
    QSignalSpy addedSpy{&context().library, &MusicLibrary::tracksAdded};
    QSignalSpy changedSpy{&context().library, &MusicLibrary::tracksMetadataChanged};

    context().library.applyExternalLibrarySnapshot(*library,
                                                   {externalTrack(u"test-source://catalog/a"_s, u"Alpha"_s),
                                                    externalTrack(u"test-source://catalog/b"_s, u"Beta"_s),
                                                    externalTrack(u"test-source://catalog/c"_s, u"Gamma"_s)},
                                                   QUuid::createUuid());

    QVariantList applied = waitForSignal(appliedSpy);
    ASSERT_EQ(applied.size(), 3);
    EXPECT_EQ(applied.at(1).toInt(), library->id);
    EXPECT_TRUE(applied.at(2).toBool());
    EXPECT_FALSE(addedSpy.isEmpty());

    auto firstTracks = tracksByPath(context().library.libraryTracks());
    ASSERT_EQ(firstTracks.size(), 3);
    for(const auto& track : firstTracks | std::views::values) {
        EXPECT_GE(track.id(), 0);
        EXPECT_EQ(track.libraryId(), library->id);
        EXPECT_TRUE(track.isEnabled());
        EXPECT_TRUE(track.isDatabaseOnlyMetadata());
    }
    EXPECT_EQ(firstTracks.at(u"test-source://catalog/a"_s).artists(),
              (QStringList{u"External Artist"_s, u"Guest Artist"_s}));

    changedSpy.clear();
    addedSpy.clear();

    context().library.applyExternalLibrarySnapshot(*library,
                                                   {externalTrack(u"test-source://catalog/a"_s, u"Alpha"_s),
                                                    externalTrack(u"test-source://catalog/b"_s, u"Beta Renamed"_s),
                                                    externalTrack(u"test-source://catalog/d"_s, u"Delta"_s)},
                                                   QUuid::createUuid());

    applied = waitForSignal(appliedSpy);
    ASSERT_EQ(applied.size(), 3);
    EXPECT_TRUE(applied.at(2).toBool());

    const auto secondTracks = tracksByPath(context().library.libraryTracks());
    ASSERT_EQ(secondTracks.size(), 3);
    EXPECT_EQ(secondTracks.at(u"test-source://catalog/a"_s).id(), firstTracks.at(u"test-source://catalog/a"_s).id());
    EXPECT_EQ(secondTracks.at(u"test-source://catalog/b"_s).id(), firstTracks.at(u"test-source://catalog/b"_s).id());
    EXPECT_EQ(secondTracks.at(u"test-source://catalog/b"_s).title(), u"Beta Renamed"_s);
    EXPECT_TRUE(secondTracks.contains(u"test-source://catalog/d"_s));
    EXPECT_FALSE(secondTracks.contains(u"test-source://catalog/c"_s));

    ASSERT_FALSE(changedSpy.isEmpty());
    const auto changedTracks = tracksByPath(changedSpy.takeLast().front().value<TrackList>());
    EXPECT_FALSE(changedTracks.contains(u"test-source://catalog/a"_s));
    ASSERT_TRUE(changedTracks.contains(u"test-source://catalog/c"_s));
    EXPECT_EQ(changedTracks.at(u"test-source://catalog/c"_s).libraryId(), -1);
    EXPECT_FALSE(changedTracks.at(u"test-source://catalog/c"_s).isEnabled());

    context().library.loadAllTracks();
    ASSERT_TRUE(waitForCondition([&]() {
        const auto reloaded = tracksByPath(context().library.libraryTracks());
        return reloaded.size() == 3 && reloaded.contains(u"test-source://catalog/d"_s);
    }));
    const auto reloadedTracks = tracksByPath(context().library.libraryTracks());
    EXPECT_EQ(reloadedTracks.at(u"test-source://catalog/a"_s).id(), firstTracks.at(u"test-source://catalog/a"_s).id());
    EXPECT_EQ(reloadedTracks.at(u"test-source://catalog/b"_s).title(), u"Beta Renamed"_s);
}

TEST_F(UnifiedMusicLibraryTest, InvalidExternalSnapshotRetainsCatalog)
{
    const auto library = context().library.addExternalLibrary(u"test-source://catalog"_s, u"External"_s);
    ASSERT_TRUE(library.has_value());

    QSignalSpy appliedSpy{&context().library, &MusicLibrary::externalLibrarySnapshotFinished};

    context().library.applyExternalLibrarySnapshot(*library, {externalTrack(u"test-source://catalog/a"_s, u"Alpha"_s)},
                                                   QUuid::createUuid());
    EXPECT_TRUE(waitForSignal(appliedSpy).at(2).toBool());
    ASSERT_EQ(context().library.libraryTracks().size(), 1);

    context().library.applyExternalLibrarySnapshot(*library, {externalTrack(u"test-source://other/a"_s, u"Outside"_s)},
                                                   QUuid::createUuid());
    EXPECT_FALSE(waitForSignal(appliedSpy).at(2).toBool());

    context().library.applyExternalLibrarySnapshot(*library,
                                                   {externalTrack(u"test-source://catalog/a"_s, u"Alpha"_s),
                                                    externalTrack(u"test-source://catalog/a"_s, u"Duplicate"_s)},
                                                   QUuid::createUuid());
    EXPECT_FALSE(waitForSignal(appliedSpy).at(2).toBool());

    LibraryInfo unknownLibrary{*library};
    unknownLibrary.id = library->id + 100;
    context().library.applyExternalLibrarySnapshot(unknownLibrary, {}, QUuid::createUuid());
    EXPECT_FALSE(waitForSignal(appliedSpy).at(2).toBool());

    const TrackList tracks = context().library.libraryTracks();
    ASSERT_EQ(tracks.size(), 1);
    EXPECT_EQ(tracks.front().title(), u"Alpha"_s);
}
TEST_F(UnifiedMusicLibraryTest, ExternalSnapshotKeepsSourceStatisticsWhenMetadataChanges)
{
    const auto library = context().library.addExternalLibrary(u"test-source://catalog"_s, u"External"_s);
    ASSERT_TRUE(library.has_value());

    QSignalSpy appliedSpy{&context().library, &MusicLibrary::externalLibrarySnapshotFinished};
    QSignalSpy loadedSpy{&context().library, &MusicLibrary::tracksLoaded};

    const QString renamedPath       = u"test-source://catalog/a"_s;
    const QString sharedOldHashPath = u"test-source://catalog/b"_s;
    const QString sharedNewHashPath = u"test-source://catalog/c"_s;
    const QString mergedPath        = u"test-source://catalog/d"_s;

    context().library.applyExternalLibrarySnapshot(
        *library,
        {externalTrack(renamedPath, u"Alpha"_s), externalTrack(sharedOldHashPath, u"Alpha"_s),
         renamedTrack(sharedNewHashPath, u"Gamma"_s, u"Gamma Artist"_s, u"Gamma Album"_s),
         externalTrack(mergedPath, u"Delta"_s)},
        QUuid::createUuid());
    ASSERT_TRUE(waitForSignal(appliedSpy).at(2).toBool());

    const auto firstTracks = tracksByPath(context().library.libraryTracks());
    ASSERT_EQ(firstTracks.size(), 4);
    const Track& firstRenamed = firstTracks.at(renamedPath);
    ASSERT_NE(firstRenamed.hash(), firstTracks.at(sharedOldHashPath).hash());

    storeTrackStats(context().dbPool, firstRenamed.hash(), 20, 100, 500, 7, 0.8F, true);
    storeTrackStats(context().dbPool, firstTracks.at(sharedNewHashPath).hash(), 10, 50, 900, 2, 0.2F, false);
    storeTrackStats(context().dbPool, firstTracks.at(mergedPath).hash(), 30, 40, 600, 4, 0.6F, true);

    context().library.loadAllTracks();
    waitForSignal(loadedSpy);
    expectTrackStats(context().library.trackForId(firstRenamed.id()), 20, 100, 500, 7, 0.8F, true);

    context().library.applyExternalLibrarySnapshot(
        *library,
        {renamedTrack(renamedPath, u"Alpha Renamed"_s, u"Renamed Artist"_s, u"Renamed Album"_s),
         externalTrack(sharedOldHashPath, u"Alpha"_s),
         renamedTrack(sharedNewHashPath, u"Gamma"_s, u"Gamma Artist"_s, u"Gamma Album"_s),
         renamedTrack(mergedPath, u"Gamma"_s, u"Gamma Artist"_s, u"Gamma Album"_s)},
        QUuid::createUuid());
    ASSERT_TRUE(waitForSignal(appliedSpy).at(2).toBool());

    UnifiedMusicLibrary reloadedLibrary{&context().libraryManager, context().dbPool,   context().playlistLoader,
                                        context().audioLoader,     context().remoteIo, &context().settings};
    QSignalSpy reloadedSpy{&reloadedLibrary, &MusicLibrary::tracksLoaded};
    reloadedLibrary.loadAllTracks();
    waitForSignal(reloadedSpy);

    const auto reloadedTracks = tracksByPath(reloadedLibrary.libraryTracks());
    ASSERT_EQ(reloadedTracks.size(), 4);

    const Track& renamed = reloadedTracks.at(renamedPath);
    EXPECT_EQ(renamed.id(), firstRenamed.id());
    EXPECT_EQ(renamed.title(), u"Alpha Renamed"_s);
    EXPECT_EQ(renamed.artists(), QStringList{u"Renamed Artist"_s});
    EXPECT_EQ(renamed.album(), u"Renamed Album"_s);
    EXPECT_EQ(renamed.hash(), firstRenamed.hash());
    expectTrackStats(renamed, 20, 100, 500, 7, 0.8F, true);

    EXPECT_EQ(reloadedTracks.at(sharedOldHashPath).playCount(), 0);
    EXPECT_FALSE(reloadedTracks.at(sharedOldHashPath).isLoved());

    const Track& merged = reloadedTracks.at(mergedPath);
    EXPECT_EQ(merged.id(), firstTracks.at(mergedPath).id());
    EXPECT_NE(merged.hash(), reloadedTracks.at(sharedNewHashPath).hash());
    expectTrackStats(merged, 30, 40, 600, 4, 0.6F, true);
    expectTrackStats(reloadedTracks.at(sharedNewHashPath), 10, 50, 900, 2, 0.2F, false);
}

TEST_F(UnifiedMusicLibraryTest, ExternalSnapshotFailsWhenSourceIsRemovedBeforeStorage)
{
    const auto library = context().library.addExternalLibrary(u"test-source://catalog"_s, u"External"_s);
    ASSERT_TRUE(library.has_value());

    QSignalSpy appliedSpy{&context().library, &MusicLibrary::externalLibrarySnapshotFinished};
    QSignalSpy addedSpy{&context().library, &MusicLibrary::tracksAdded};

    context().library.applyExternalLibrarySnapshot(*library, {}, QUuid::createUuid());
    ASSERT_TRUE(waitForSignal(appliedSpy).at(2).toBool());
    // The previous commit releases the queue with a queued call, so the next snapshot then starts synchronously and
    // waits for the database lock below.
    QCoreApplication::processEvents();

    const DbConnectionProvider dbProvider{context().dbPool};
    DbQuery lockQuery{dbProvider.db(), u"BEGIN EXCLUSIVE;"_s};
    ASSERT_TRUE(lockQuery.exec()) << lockQuery.lastError().text().toStdString();

    context().library.applyExternalLibrarySnapshot(*library,
                                                   {externalTrack(u"test-source://catalog/a"_s, u"Alpha"_s),
                                                    externalTrack(u"test-source://catalog/b"_s, u"Beta"_s)},
                                                   QUuid::createUuid());

    ASSERT_TRUE(context().libraryManager.removeLibrary(library->id));

    DbQuery unlockQuery{dbProvider.db(), u"COMMIT;"_s};
    ASSERT_TRUE(unlockQuery.exec()) << unlockQuery.lastError().text().toStdString();

    const QVariantList applied = waitForSignal(appliedSpy);
    ASSERT_EQ(applied.size(), 3);
    EXPECT_EQ(applied.at(1).toInt(), library->id);
    EXPECT_FALSE(applied.at(2).toBool());

    QCoreApplication::processEvents();
    EXPECT_TRUE(addedSpy.isEmpty());
    EXPECT_TRUE(context().library.tracks().empty());
    EXPECT_EQ(trackRowCount(context().dbPool, u"FilePath LIKE :value"_s, u"test-source://catalog/%"_s), 0);

    QSignalSpy loadedSpy{&context().library, &MusicLibrary::tracksLoaded};
    context().library.loadAllTracks();
    waitForSignal(loadedSpy);
    EXPECT_TRUE(context().library.tracks().empty());
}

TEST_F(UnifiedMusicLibraryTest, ExternalSnapshotIsNotPublishedWhenSourceIsRemovedAfterStorage)
{
    const auto library = context().library.addExternalLibrary(u"test-source://catalog"_s, u"External"_s);
    ASSERT_TRUE(library.has_value());

    QSignalSpy appliedSpy{&context().library, &MusicLibrary::externalLibrarySnapshotFinished};

    context().library.applyExternalLibrarySnapshot(*library,
                                                   {externalTrack(u"test-source://catalog/a"_s, u"Alpha"_s),
                                                    externalTrack(u"test-source://catalog/b"_s, u"Beta"_s)},
                                                   QUuid::createUuid());
    ASSERT_TRUE(waitForSignal(appliedSpy).at(2).toBool());
    const int detachedId = tracksByPath(context().library.libraryTracks()).at(u"test-source://catalog/b"_s).id();

    QSignalSpy addedSpy{&context().library, &MusicLibrary::tracksAdded};
    QSignalSpy changedSpy{&context().library, &MusicLibrary::tracksMetadataChanged};

    // The previous commit releases the queue with a queued call, so the next snapshot then starts synchronously.
    QCoreApplication::processEvents();

    context().library.applyExternalLibrarySnapshot(*library,
                                                   {externalTrack(u"test-source://catalog/a"_s, u"Alpha Renamed"_s),
                                                    externalTrack(u"test-source://catalog/c"_s, u"Gamma"_s)},
                                                   QUuid::createUuid());

    QElapsedTimer timer;
    timer.start();
    while(trackRowCount(context().dbPool, u"FilePath = :value"_s, u"test-source://catalog/c"_s) == 0
          && timer.elapsed() < 10000) {
        QThread::msleep(5);
    }
    ASSERT_EQ(trackRowCount(context().dbPool, u"FilePath = :value"_s, u"test-source://catalog/c"_s), 1);
    EXPECT_TRUE(appliedSpy.isEmpty());

    ASSERT_TRUE(context().libraryManager.removeLibrary(library->id));

    const QVariantList applied = waitForSignal(appliedSpy);
    ASSERT_EQ(applied.size(), 3);
    EXPECT_EQ(applied.at(1).toInt(), library->id);
    EXPECT_FALSE(applied.at(2).toBool());

    ASSERT_TRUE(waitForCondition([&]() { return context().library.libraryTracks().empty(); }));
    EXPECT_TRUE(addedSpy.isEmpty());
    for(const QVariantList& changed : changedSpy) {
        for(const Track& track : changed.front().value<TrackList>()) {
            EXPECT_NE(track.title(), u"Alpha Renamed"_s);
        }
    }

    EXPECT_EQ(trackRowCount(context().dbPool, u"LibraryID = :value"_s, library->id), 0);
    EXPECT_EQ(trackRowCount(context().dbPool, u"FilePath LIKE :value"_s, u"test-source://catalog/%"_s), 1);

    QSignalSpy loadedSpy{&context().library, &MusicLibrary::tracksLoaded};
    context().library.loadAllTracks();
    waitForSignal(loadedSpy);
    EXPECT_TRUE(context().library.libraryTracks().empty());

    const TrackList tracks = context().library.tracks();
    ASSERT_EQ(tracks.size(), 1);
    EXPECT_EQ(tracks.front().id(), detachedId);
    EXPECT_EQ(tracks.front().filepath(), u"test-source://catalog/b"_s);
    EXPECT_EQ(tracks.front().libraryId(), -1);
}

TEST_F(UnifiedMusicLibraryTest, ExternalSnapshotNotificationsSeeTheFinalCatalog)
{
    const auto library = context().library.addExternalLibrary(u"test-source://catalog"_s, u"External"_s);
    ASSERT_TRUE(library.has_value());

    QSignalSpy appliedSpy{&context().library, &MusicLibrary::externalLibrarySnapshotFinished};

    context().library.applyExternalLibrarySnapshot(*library,
                                                   {externalTrack(u"test-source://catalog/a"_s, u"Alpha"_s),
                                                    externalTrack(u"test-source://catalog/b"_s, u"Beta"_s),
                                                    externalTrack(u"test-source://catalog/c"_s, u"Gamma"_s)},
                                                   QUuid::createUuid());
    ASSERT_TRUE(waitForSignal(appliedSpy).at(2).toBool());

    std::vector<std::map<QString, QString>> notifiedCatalogs;
    const auto captureCatalog = [this, &notifiedCatalogs]() {
        notifiedCatalogs.push_back(titlesByPath(context().library.libraryTracks()));
    };
    const auto addedConnection
        = QObject::connect(&context().library, &MusicLibrary::tracksAdded, &context().library, captureCatalog);
    const auto changedConnection = QObject::connect(&context().library, &MusicLibrary::tracksMetadataChanged,
                                                    &context().library, captureCatalog);
    const auto updatedConnection
        = QObject::connect(&context().library, &MusicLibrary::tracksUpdated, &context().library, captureCatalog);
    const auto deletedConnection
        = QObject::connect(&context().library, &MusicLibrary::tracksDeleted, &context().library, captureCatalog);

    context().library.applyExternalLibrarySnapshot(*library,
                                                   {externalTrack(u"test-source://catalog/a"_s, u"Alpha"_s),
                                                    externalTrack(u"test-source://catalog/b"_s, u"Beta Renamed"_s),
                                                    externalTrack(u"test-source://catalog/d"_s, u"Delta"_s)},
                                                   QUuid::createUuid());
    ASSERT_TRUE(waitForSignal(appliedSpy).at(2).toBool());

    QObject::disconnect(addedConnection);
    QObject::disconnect(changedConnection);
    QObject::disconnect(updatedConnection);
    QObject::disconnect(deletedConnection);

    const std::map<QString, QString> finalCatalog{
        {u"test-source://catalog/a"_s, u"Alpha"_s},
        {u"test-source://catalog/b"_s, u"Beta Renamed"_s},
        {u"test-source://catalog/d"_s, u"Delta"_s},
    };
    EXPECT_EQ(titlesByPath(context().library.libraryTracks()), finalCatalog);

    ASSERT_EQ(notifiedCatalogs.size(), 2);
    for(const auto& catalog : notifiedCatalogs) {
        EXPECT_EQ(catalog, finalCatalog);
    }
}

TEST_F(UnifiedMusicLibraryTest, ExternalLibraryRegistrationRejectsOverlappingPaths)
{
    const auto child = context().library.addExternalLibrary(u"test-source://catalog/child"_s, u"Child"_s);
    ASSERT_TRUE(child.has_value());
    EXPECT_FALSE(context().library.addExternalLibrary(u"test-source://catalog"_s, u"Parent"_s).has_value());

    const auto parent = context().library.addExternalLibrary(u"test-source://other"_s, u"Other"_s);
    ASSERT_TRUE(parent.has_value());
    EXPECT_FALSE(context().library.addExternalLibrary(u"test-source://other/child"_s, u"Other Child"_s).has_value());

    EXPECT_TRUE(context().library.addExternalLibrary(u"test-source://catalog/children"_s, u"Sibling"_s).has_value());
}

TEST_F(UnifiedMusicLibraryTest, ExternalSnapshotCannotTakeTracksOfAnOverlappingLibrary)
{
    const auto child = context().library.addExternalLibrary(u"test-source://catalog/child"_s, u"Child"_s);
    ASSERT_TRUE(child.has_value());
    const auto other = context().library.addExternalLibrary(u"test-source://other"_s, u"Other"_s);
    ASSERT_TRUE(other.has_value());

    QSignalSpy appliedSpy{&context().library, &MusicLibrary::externalLibrarySnapshotFinished};

    context().library.applyExternalLibrarySnapshot(
        *child, {externalTrack(u"test-source://catalog/child/a"_s, u"Child A"_s)}, QUuid::createUuid());
    ASSERT_TRUE(waitForSignal(appliedSpy).at(2).toBool());
    context().library.applyExternalLibrarySnapshot(
        *other, {externalTrack(u"test-source://other/child/a"_s, u"Other A"_s)}, QUuid::createUuid());
    ASSERT_TRUE(waitForSignal(appliedSpy).at(2).toBool());

    const int parentId     = insertLibrary(context().dbPool, u"test-source://catalog"_s, u"Parent"_s);
    const int otherChildId = insertLibrary(context().dbPool, u"test-source://other/child"_s, u"Other Child"_s);
    context().libraryManager.reset();
    const auto parent = context().libraryManager.libraryInfo(parentId);
    ASSERT_TRUE(parent.has_value());
    const auto otherChild = context().libraryManager.libraryInfo(otherChildId);
    ASSERT_TRUE(otherChild.has_value());

    const auto initialTracks = tracksByPath(context().library.libraryTracks());
    ASSERT_EQ(initialTracks.size(), 2);

    context().library.applyExternalLibrarySnapshot(
        *parent, {externalTrack(u"test-source://catalog/child/a"_s, u"Taken By Parent"_s)}, QUuid::createUuid());
    EXPECT_FALSE(waitForSignal(appliedSpy).at(2).toBool());

    context().library.applyExternalLibrarySnapshot(
        *otherChild, {externalTrack(u"test-source://other/child/a"_s, u"Taken By Child"_s)}, QUuid::createUuid());
    EXPECT_FALSE(waitForSignal(appliedSpy).at(2).toBool());

    context().library.applyExternalLibrarySnapshot(
        *parent, {externalTrack(u"test-source://catalog/p"_s, u"Parent P"_s)}, QUuid::createUuid());
    EXPECT_TRUE(waitForSignal(appliedSpy).at(2).toBool());
    context().library.applyExternalLibrarySnapshot(
        *otherChild, {externalTrack(u"test-source://other/child/b"_s, u"Child B"_s)}, QUuid::createUuid());
    EXPECT_TRUE(waitForSignal(appliedSpy).at(2).toBool());

    QSignalSpy loadedSpy{&context().library, &MusicLibrary::tracksLoaded};
    context().library.loadAllTracks();
    waitForSignal(loadedSpy);

    const auto tracks = tracksByPath(context().library.libraryTracks());
    ASSERT_EQ(tracks.size(), 4);
    for(const QString& path : {u"test-source://catalog/child/a"_s, u"test-source://other/child/a"_s}) {
        const Track& track = tracks.at(path);
        EXPECT_EQ(track.id(), initialTracks.at(path).id());
        EXPECT_EQ(track.libraryId(), initialTracks.at(path).libraryId());
        EXPECT_EQ(track.title(), initialTracks.at(path).title());
        EXPECT_TRUE(track.isEnabled());
    }
    EXPECT_EQ(tracks.at(u"test-source://catalog/p"_s).libraryId(), parentId);
    EXPECT_EQ(tracks.at(u"test-source://other/child/b"_s).libraryId(), otherChildId);
}

TEST_F(UnifiedMusicLibraryTest, ExternalSnapshotStorageRejectsTracksOfOtherLibraries)
{
    const auto library = context().library.addExternalLibrary(u"test-source://catalog"_s, u"External"_s);
    ASSERT_TRUE(library.has_value());
    const auto other = context().library.addExternalLibrary(u"test-source://other"_s, u"Other"_s);
    ASSERT_TRUE(other.has_value());

    QSignalSpy appliedSpy{&context().library, &MusicLibrary::externalLibrarySnapshotFinished};
    context().library.applyExternalLibrarySnapshot(*other, {externalTrack(u"test-source://other/a"_s, u"Other A"_s)},
                                                   QUuid::createUuid());
    ASSERT_TRUE(waitForSignal(appliedSpy).at(2).toBool());
    const Track otherTrack = context().library.libraryTracks().front();

    TrackDatabase trackDatabase;
    trackDatabase.initialise(DbConnectionProvider{context().dbPool});

    Track claimedPath{otherTrack};
    claimedPath.setId(-1);
    claimedPath.setLibraryId(library->id);
    TrackList addedTracks{claimedPath};
    TrackList noTracks;
    EXPECT_FALSE(trackDatabase.storeExternalSnapshot(*library, addedTracks, noTracks, Track::Stat::None));

    Track claimedId{otherTrack};
    claimedId.setLibraryId(library->id);
    claimedId.setTitle(u"Claimed"_s);
    TrackList updatedTracks{claimedId};
    EXPECT_FALSE(trackDatabase.storeExternalSnapshot(*library, noTracks, updatedTracks, Track::Stat::None));

    LibraryInfo removedLibrary{*library};
    removedLibrary.id = library->id + 100;
    TrackList newTracks{externalTrack(u"test-source://catalog/new"_s, u"New"_s)};
    newTracks.front().setLibraryId(removedLibrary.id);
    newTracks.front().generateHash();
    EXPECT_FALSE(trackDatabase.storeExternalSnapshot(removedLibrary, newTracks, noTracks, Track::Stat::None));

    EXPECT_EQ(trackRowCount(context().dbPool, u"LibraryID = :value"_s, other->id), 1);
    EXPECT_EQ(trackRowCount(context().dbPool, u"LibraryID = :value"_s, library->id), 0);
    EXPECT_EQ(trackRowCount(context().dbPool, u"FilePath = :value"_s, u"test-source://catalog/new"_s), 0);

    QSignalSpy loadedSpy{&context().library, &MusicLibrary::tracksLoaded};
    context().library.loadAllTracks();
    waitForSignal(loadedSpy);
    const TrackList tracks = context().library.libraryTracks();
    ASSERT_EQ(tracks.size(), 1);
    EXPECT_EQ(tracks.front().title(), u"Other A"_s);
    EXPECT_EQ(tracks.front().libraryId(), other->id);
}
TEST_F(UnifiedMusicLibraryTest, ExternalLovedIsAuthoritativeIsolatedAndProtectedAcrossReload)
{
    const QString localPath        = createTrackFile(u"local.mp3"_s, u"Same title"_s);
    const LibraryInfo localLibrary = addLibrary(u"Local"_s);
    waitForSuccessfulScan([&]() { return context().library.rescan(localLibrary); });
    ASSERT_EQ(context().library.libraryTracks().size(), 1U);
    Track local = context().library.libraryTracks().front();
    local.setLoved(true);
    context().library.updateTrackStats(local, Track::Stat::Loved);
    ASSERT_TRUE(waitForCondition([&]() { return context().library.trackForId(local.id()).isLoved(); }));
    const auto accountA = context().library.addExternalLibrary(u"test-source://account-a"_s, u"Account A"_s);
    const auto accountB = context().library.addExternalLibrary(u"test-source://account-b"_s, u"Account B"_s);
    ASSERT_TRUE(accountA);
    ASSERT_TRUE(accountB);
    context().library.setExternalLibraryReadOnlyStats(*accountA, Track::Stat::Loved);
    context().library.setExternalLibraryReadOnlyStats(*accountB, Track::Stat::Loved);
    Track a{local};
    a.setId(-1);
    a.setFilePath(u"test-source://account-a/item"_s);
    Track b{a};
    b.setFilePath(u"test-source://account-b/item"_s);
    b.setLoved(false);
    a.setLoved(true);
    QSignalSpy applied{&context().library, &MusicLibrary::externalLibrarySnapshotFinished};
    context().library.applyExternalLibrarySnapshot(*accountA, {a}, QUuid::createUuid());
    ASSERT_TRUE(waitForSignal(applied).at(2).toBool());
    context().library.applyExternalLibrarySnapshot(*accountB, {b}, QUuid::createUuid());
    ASSERT_TRUE(waitForSignal(applied).at(2).toBool());
    auto tracks         = tracksByPath(context().library.libraryTracks());
    const Track storedA = tracks.at(a.filepath());
    const Track storedB = tracks.at(b.filepath());
    EXPECT_NE(storedA.hash(), storedB.hash());
    EXPECT_NE(storedA.hash(), context().library.trackForId(local.id()).hash());
    EXPECT_TRUE(context().library.trackForId(local.id()).isLoved());
    local.setLoved(false);
    context().library.updateTrackStats(local, Track::Stat::Loved);
    ASSERT_TRUE(waitForCondition([&]() { return !context().library.trackForId(local.id()).isLoved(); }));
    EXPECT_TRUE(storedA.isLoved());
    EXPECT_FALSE(storedB.isLoved());
    EXPECT_FALSE(context().library.canUpdateTrackStats(storedA, Track::Stat::Loved));
    EXPECT_TRUE(context().library.canUpdateTrackStats(storedA, Track::Stat::Rating));

    Track forged{storedA};
    forged.setLoved(false);
    forged.setReadOnlyStats(Track::Stat::None);
    forged.setExternalLibraryPath({});
    context().library.updateTrackStats(forged, Track::Stat::Loved);
    context().library.updateTrackMetadata({forged});
    context().library.applyExternalLibrarySnapshot(*accountB, {b}, QUuid::createUuid());
    ASSERT_TRUE(waitForSignal(applied).at(2).toBool());
    EXPECT_TRUE(context().library.trackForId(storedA.id()).isLoved());
    EXPECT_FALSE(context().library.trackForId(local.id()).isLoved());

    a.setLoved(false);
    context().library.applyExternalLibrarySnapshot(*accountA, {a}, QUuid::createUuid());
    ASSERT_TRUE(waitForSignal(applied).at(2).toBool());
    EXPECT_FALSE(context().library.trackForId(storedA.id()).isLoved());
    EXPECT_EQ(context().library.trackForId(local.id()).filepath(), localPath);
    EXPECT_FALSE(context().library.trackForId(local.id()).isLoved());
    QSignalSpy loaded{&context().library, &MusicLibrary::tracksLoaded};
    context().library.loadAllTracks();
    waitForSignal(loaded);
    EXPECT_FALSE(context().library.trackForId(storedA.id()).isLoved());
    EXPECT_FALSE(context().library.canUpdateTrackStats(context().library.trackForId(storedA.id()), Track::Stat::Loved));
}

TEST_F(UnifiedMusicLibraryTest, ExternalSnapshotReplacesSourceOwnedStatisticsAndPreservesLocalStatistics)
{
    const auto owned = context().library.addExternalLibrary(u"test-source://owned"_s, u"Owned"_s);
    const auto loved = context().library.addExternalLibrary(u"test-source://loved"_s, u"Loved"_s);
    ASSERT_TRUE(owned);
    ASSERT_TRUE(loved);
    context().library.setExternalLibraryReadOnlyStats(*owned, Track::Stat::Rating | Track::Stat::Playcount);
    context().library.setExternalLibraryReadOnlyStats(*loved, Track::Stat::Loved);

    Track ownedSource = externalTrack(u"test-source://owned/item"_s, u"Owned"_s);
    ownedSource.setRating(0.4F);
    ownedSource.setPlayCount(9);
    ownedSource.setFirstPlayed(100);
    ownedSource.setLastPlayed(500);
    Track lovedSource = externalTrack(u"test-source://loved/item"_s, u"Loved"_s);
    lovedSource.setLoved(true);

    QSignalSpy applied{&context().library, &MusicLibrary::externalLibrarySnapshotFinished};
    context().library.applyExternalLibrarySnapshot(*owned, {ownedSource}, QUuid::createUuid());
    ASSERT_TRUE(waitForSignal(applied).at(2).toBool());
    context().library.applyExternalLibrarySnapshot(*loved, {lovedSource}, QUuid::createUuid());
    ASSERT_TRUE(waitForSignal(applied).at(2).toBool());

    const auto imported    = tracksByPath(context().library.libraryTracks());
    const Track ownedTrack = imported.at(ownedSource.filepath());
    const Track lovedTrack = imported.at(lovedSource.filepath());
    EXPECT_EQ(ownedTrack.playCount(), 9);
    EXPECT_FLOAT_EQ(ownedTrack.rating(), 0.4F);
    EXPECT_FALSE(context().library.canUpdateTrackStats(ownedTrack, Track::Stat::Rating));
    EXPECT_FALSE(context().library.canUpdateTrackStats(ownedTrack, Track::Stat::Playcount));
    ASSERT_TRUE(context().library.canUpdateTrackStats(ownedTrack, Track::Stat::Loved));

    Track ownedLocal{ownedTrack};
    ownedLocal.setLoved(true);
    context().library.updateTrackStats(ownedLocal, Track::Stat::Loved);
    Track lovedLocal{lovedTrack};
    lovedLocal.setRating(0.6F);
    lovedLocal.setPlayCount(5);
    lovedLocal.setFirstPlayed(40);
    lovedLocal.setLastPlayed(900);
    context().library.updateTrackStats(lovedLocal, Track::Stat::Rating | Track::Stat::Playcount);
    ASSERT_TRUE(waitForCondition([&]() {
        const Track resident = context().library.trackForId(lovedTrack.id());
        return context().library.trackForId(ownedTrack.id()).isLoved() && resident.playCount() == 5
            && resident.rating() == 0.6F;
    }));

    // The play time setters only move earlier or later, so the later snapshot uses new source tracks.
    ownedSource = externalTrack(u"test-source://owned/item"_s, u"Owned"_s);
    ownedSource.setRating(0.8F);
    ownedSource.setPlayCount(3);
    ownedSource.setFirstPlayed(200);
    ownedSource.setLastPlayed(300);
    ownedSource.setLoved(false);
    lovedSource = externalTrack(u"test-source://loved/item"_s, u"Loved"_s);
    lovedSource.setRating(0.2F);
    lovedSource.setPlayCount(1);
    lovedSource.setFirstPlayed(10);
    lovedSource.setLastPlayed(20);
    lovedSource.setLoved(false);
    context().library.applyExternalLibrarySnapshot(*owned, {ownedSource}, QUuid::createUuid());
    ASSERT_TRUE(waitForSignal(applied).at(2).toBool());
    context().library.applyExternalLibrarySnapshot(*loved, {lovedSource}, QUuid::createUuid());
    ASSERT_TRUE(waitForSignal(applied).at(2).toBool());

    const auto expectOwned = [](const Track& track) {
        EXPECT_FLOAT_EQ(track.rating(), 0.8F);
        EXPECT_EQ(track.playCount(), 3);
        EXPECT_EQ(track.firstPlayed(), 200U);
        EXPECT_EQ(track.lastPlayed(), 300U);
        EXPECT_TRUE(track.isLoved());
    };
    const auto expectLoved = [](const Track& track) {
        EXPECT_FLOAT_EQ(track.rating(), 0.6F);
        EXPECT_EQ(track.playCount(), 5);
        EXPECT_EQ(track.firstPlayed(), 40U);
        EXPECT_EQ(track.lastPlayed(), 900U);
        EXPECT_FALSE(track.isLoved());
    };

    expectOwned(context().library.trackForId(ownedTrack.id()));
    expectLoved(context().library.trackForId(lovedTrack.id()));

    TrackDatabase database;
    database.initialise(DbConnectionProvider{context().dbPool});
    Track ownedStored{ownedTrack};
    ASSERT_TRUE(database.reloadTrack(ownedStored));
    expectOwned(ownedStored);
    Track lovedStored{lovedTrack};
    ASSERT_TRUE(database.reloadTrack(lovedStored));
    expectLoved(lovedStored);

    UnifiedMusicLibrary reloaded{&context().libraryManager, context().dbPool,   context().playlistLoader,
                                 context().audioLoader,     context().remoteIo, &context().settings};
    QSignalSpy loaded{&reloaded, &MusicLibrary::tracksLoaded};
    reloaded.loadAllTracks();
    waitForSignal(loaded);
    expectOwned(reloaded.trackForId(ownedTrack.id()));
    expectLoved(reloaded.trackForId(lovedTrack.id()));
    EXPECT_FALSE(reloaded.canUpdateTrackStats(reloaded.trackForId(ownedTrack.id()), Track::Stat::Playcount));
}

TEST_F(UnifiedMusicLibraryTest, ExternalActivationRetainsTrackIdentityAndPersistsIndependentlyOfFilters)
{
    const auto library = context().library.addExternalLibrary(u"test-source://account"_s, u"Account"_s);
    ASSERT_TRUE(library);
    QSignalSpy applied{&context().library, &MusicLibrary::externalLibrarySnapshotFinished};
    context().library.applyExternalLibrarySnapshot(
        *library, {externalTrack(u"test-source://account/item"_s, u"Title"_s)}, QUuid::createUuid());
    ASSERT_TRUE(waitForSignal(applied).at(2).toBool());
    const Track track = context().library.libraryTracks().front();
    context().library.setExternalLibraryActive(*library, false);
    EXPECT_TRUE(context().library.activeLibraryTracks().empty());
    EXPECT_TRUE(context().library.visibleLibraryTracks().empty());
    EXPECT_EQ(context().library.trackForId(track.id()).filepath(), track.filepath());
    EXPECT_EQ(context().library.libraryTracks().size(), 1U);
    context().library.clearActiveLibraryFilters();
    EXPECT_TRUE(context().library.visibleLibraryTracks().empty());
    UnifiedMusicLibrary reloaded{&context().libraryManager, context().dbPool,   context().playlistLoader,
                                 context().audioLoader,     context().remoteIo, &context().settings};
    QSignalSpy loaded{&reloaded, &MusicLibrary::tracksLoaded};
    reloaded.loadAllTracks();
    waitForSignal(loaded);
    EXPECT_TRUE(reloaded.activeLibraryTracks().empty());
    EXPECT_EQ(reloaded.trackForId(track.id()).filepath(), track.filepath());
    reloaded.setExternalLibraryActive(*library, true);
    ASSERT_EQ(reloaded.activeLibraryTracks().size(), 1U);
    EXPECT_EQ(reloaded.activeLibraryTracks().front().id(), track.id());
}

TEST_F(UnifiedMusicLibraryTest, RemovedExternalLibraryClearsOnlyItsCommittedSourceState)
{
    const QString removedPath{u"test-source://removed"_s};
    const QString keptPath{u"test-source://kept"_s};
    const auto removed = context().library.addExternalLibrary(removedPath, u"Removed"_s);
    const auto kept    = context().library.addExternalLibrary(keptPath, u"Kept"_s);
    ASSERT_TRUE(removed);
    ASSERT_TRUE(kept);
    context().library.setExternalLibraryReadOnlyStats(*removed, Track::Stat::Loved);
    context().library.setExternalLibraryReadOnlyStats(*kept, Track::Stat::Loved);
    QSignalSpy applied{&context().library, &MusicLibrary::externalLibrarySnapshotFinished};
    context().library.applyExternalLibrarySnapshot(*removed, {externalTrack(removedPath + u"/item"_s, u"Removed"_s)},
                                                   QUuid::createUuid());
    ASSERT_TRUE(waitForSignal(applied).at(2).toBool());
    context().library.applyExternalLibrarySnapshot(*kept, {externalTrack(keptPath + u"/item"_s, u"Kept"_s)},
                                                   QUuid::createUuid());
    ASSERT_TRUE(waitForSignal(applied).at(2).toBool());
    const auto byPath        = tracksByPath(context().library.libraryTracks());
    const Track removedTrack = byPath.at(removedPath + u"/item"_s);
    const Track keptTrack    = byPath.at(keptPath + u"/item"_s);
    ASSERT_EQ(removedTrack.readOnlyStats(), Track::Stats{Track::Stat::Loved});
    // A playlist entry keeps the removed track as a detached record.
    ASSERT_NE(context().playlistHandler.createPlaylist(u"Local"_s, {removedTrack}), nullptr);
    context().playlistHandler.savePlaylists();
    context().library.setExternalLibraryActive(*removed, false);
    context().library.setExternalLibraryActive(*kept, false);

    const QString removedInactiveKey{u"ExternalLibraries/Inactive/"_s + removedPath};
    const QString removedStatsKey{u"ExternalLibraries/ReadOnlyStats/"_s + removedPath};
    const QString keptInactiveKey{u"ExternalLibraries/Inactive/"_s + keptPath};
    const QString keptStatsKey{u"ExternalLibraries/ReadOnlyStats/"_s + keptPath};
    ASSERT_TRUE(context().settings.fileContains(removedInactiveKey));
    ASSERT_TRUE(context().settings.fileContains(removedStatsKey));

    const DbConnectionProvider dbProvider{context().dbPool};
    DbQuery failDelete{dbProvider.db(), u"CREATE TRIGGER FailLibraries BEFORE DELETE ON Libraries "
                                        u"BEGIN SELECT RAISE(ABORT, 'synthetic failure'); END;"_s};
    ASSERT_TRUE(failDelete.exec()) << failDelete.lastError().text().toStdString();
    EXPECT_FALSE(context().libraryManager.removeLibrary(removed->id));
    QCoreApplication::processEvents();
    EXPECT_TRUE(context().settings.fileContains(removedInactiveKey));
    EXPECT_TRUE(context().settings.fileContains(removedStatsKey));
    EXPECT_FALSE(context().library.isTrackSourceActive(context().library.trackForId(removedTrack.id())));
    EXPECT_EQ(context().library.trackForId(removedTrack.id()).readOnlyStats(), Track::Stats{Track::Stat::Loved});
    DbQuery dropDelete{dbProvider.db(), u"DROP TRIGGER FailLibraries;"_s};
    ASSERT_TRUE(dropDelete.exec()) << dropDelete.lastError().text().toStdString();

    TrackDatabase database;
    database.initialise(DbConnectionProvider{context().dbPool});
    QSignalSpy activation{&context().library, &MusicLibrary::sourceActivationChanged};
    ASSERT_TRUE(context().libraryManager.removeLibrary(removed->id));
    EXPECT_FALSE(context().settings.fileContains(removedInactiveKey));
    EXPECT_FALSE(context().settings.fileContains(removedStatsKey));
    EXPECT_EQ(activation.size(), 1);
    EXPECT_TRUE(context().settings.fileContains(keptInactiveKey));
    EXPECT_TRUE(context().settings.fileContains(keptStatsKey));
    Track stored{removedTrack};
    ASSERT_TRUE(waitForCondition(
        [&]() { return database.reloadTrack(stored) && stored.readOnlyStats() == Track::Stat::None; }));
    EXPECT_EQ(stored.libraryId(), -1);
    const auto expectDetached = [&removedTrack, &keptTrack](const MusicLibrary& library) {
        const Track detached = library.trackForId(removedTrack.id());
        EXPECT_EQ(detached.filepath(), removedTrack.filepath());
        EXPECT_EQ(detached.libraryId(), -1);
        EXPECT_EQ(detached.readOnlyStats(), Track::Stats{Track::Stat::None});
        EXPECT_TRUE(library.isTrackSourceActive(detached));
        EXPECT_TRUE(library.canUpdateTrackStats(detached, Track::Stat::Loved));
        const Track other = library.trackForId(keptTrack.id());
        EXPECT_EQ(other.readOnlyStats(), Track::Stats{Track::Stat::Loved});
        EXPECT_FALSE(library.isTrackSourceActive(other));
        EXPECT_FALSE(library.canUpdateTrackStats(other, Track::Stat::Loved));
    };
    expectDetached(context().library);
    UnifiedMusicLibrary reloaded{&context().libraryManager, context().dbPool,   context().playlistLoader,
                                 context().audioLoader,     context().remoteIo, &context().settings};
    QSignalSpy loaded{&reloaded, &MusicLibrary::tracksLoaded};
    reloaded.loadAllTracks();
    waitForSignal(loaded);
    expectDetached(reloaded);

    const auto readded = context().library.addExternalLibrary(removedPath, u"Removed"_s);
    ASSERT_TRUE(readded);
    EXPECT_FALSE(context().settings.fileContains(u"ExternalLibraries/Inactive/"_s + readded->path));
    EXPECT_FALSE(context().settings.fileContains(u"ExternalLibraries/ReadOnlyStats/"_s + readded->path));
    context().library.applyExternalLibrarySnapshot(*readded, {externalTrack(removedPath + u"/item"_s, u"Removed"_s)},
                                                   QUuid::createUuid());
    ASSERT_TRUE(waitForSignal(applied).at(2).toBool());
    TrackList readdedTracks;
    std::ranges::copy_if(context().library.libraryTracks(), std::back_inserter(readdedTracks),
                         [&readded](const Track& track) { return track.libraryId() == readded->id; });
    ASSERT_EQ(readdedTracks.size(), 1U);
    EXPECT_EQ(readdedTracks.front().filepath(), removedTrack.filepath());
    EXPECT_EQ(readdedTracks.front().readOnlyStats(), Track::Stats{Track::Stat::None});
    EXPECT_TRUE(context().library.isTrackSourceActive(readdedTracks.front()));
    EXPECT_TRUE(context().library.canUpdateTrackStats(readdedTracks.front(), Track::Stat::Loved));
}

TEST_F(UnifiedMusicLibraryTest, ExternalLibraryRemovalFailureKeepsAllSourceStateForRetry)
{
    const QString removedPath{u"test-source://removed"_s};
    const QString keptPath{u"test-source://kept"_s};
    const auto removed = context().library.addExternalLibrary(removedPath, u"Removed"_s);
    const auto kept    = context().library.addExternalLibrary(keptPath, u"Kept"_s);
    ASSERT_TRUE(removed);
    ASSERT_TRUE(kept);
    context().library.setExternalLibraryReadOnlyStats(*removed, Track::Stat::Loved);
    context().library.setExternalLibraryReadOnlyStats(*kept, Track::Stat::Loved);
    QSignalSpy applied{&context().library, &MusicLibrary::externalLibrarySnapshotFinished};
    context().library.applyExternalLibrarySnapshot(*removed,
                                                   {externalTrack(removedPath + u"/listed"_s, u"Listed"_s),
                                                    externalTrack(removedPath + u"/detached"_s, u"Detached"_s)},
                                                   QUuid::createUuid());
    ASSERT_TRUE(waitForSignal(applied).at(2).toBool());
    // The second snapshot detaches a record that keeps the source path and its source-owned statistics.
    context().library.applyExternalLibrarySnapshot(*removed, {externalTrack(removedPath + u"/listed"_s, u"Listed"_s)},
                                                   QUuid::createUuid());
    ASSERT_TRUE(waitForSignal(applied).at(2).toBool());
    context().library.applyExternalLibrarySnapshot(*kept, {externalTrack(keptPath + u"/item"_s, u"Kept"_s)},
                                                   QUuid::createUuid());
    ASSERT_TRUE(waitForSignal(applied).at(2).toBool());
    const auto byPath       = tracksByPath(context().library.libraryTracks());
    const Track listedTrack = byPath.at(removedPath + u"/listed"_s);
    const Track keptTrack   = byPath.at(keptPath + u"/item"_s);
    ASSERT_NE(context().playlistHandler.createPlaylist(u"Local"_s, {listedTrack}), nullptr);
    context().playlistHandler.savePlaylists();
    context().library.setExternalLibraryActive(*removed, false);

    TrackDatabase database;
    database.initialise(DbConnectionProvider{context().dbPool});
    Track detachedTrack;
    for(const Track& track : database.getAllTracks()) {
        if(track.filepath() == removedPath + u"/detached"_s) {
            detachedTrack = track;
        }
    }
    ASSERT_GE(detachedTrack.id(), 0);
    ASSERT_EQ(detachedTrack.libraryId(), -1);
    ASSERT_EQ(detachedTrack.readOnlyStats(), Track::Stats{Track::Stat::Loved});

    const QString removedInactiveKey{u"ExternalLibraries/Inactive/"_s + removedPath};
    const QString removedStatsKey{u"ExternalLibraries/ReadOnlyStats/"_s + removedPath};
    const QString keptStatsKey{u"ExternalLibraries/ReadOnlyStats/"_s + keptPath};
    const auto expectStored = [&database](const Track& track, int libraryId, Track::Stats readOnlyStats) {
        Track stored{track};
        ASSERT_TRUE(database.reloadTrack(stored));
        EXPECT_EQ(stored.libraryId(), libraryId);
        EXPECT_EQ(stored.readOnlyStats(), readOnlyStats);
    };
    const auto expectInMemory
        = [](const MusicLibrary& library, const Track& track, int libraryId, Track::Stats readOnlyStats) {
              const Track current = library.trackForId(track.id());
              EXPECT_EQ(current.filepath(), track.filepath());
              EXPECT_EQ(current.libraryId(), libraryId);
              EXPECT_EQ(current.readOnlyStats(), readOnlyStats);
          };

    const DbConnectionProvider dbProvider{context().dbPool};
    DbQuery failUpdate{dbProvider.db(), u"CREATE TRIGGER FailExtraProperties BEFORE UPDATE OF ExtraProperties ON "
                                        u"Tracks BEGIN SELECT RAISE(ABORT, 'synthetic failure'); END;"_s};
    ASSERT_TRUE(failUpdate.exec()) << failUpdate.lastError().text().toStdString();
    QSignalSpy libraryRemoved{&context().libraryManager, &LibraryManager::libraryRemoved};
    QSignalSpy activation{&context().library, &MusicLibrary::sourceActivationChanged};
    EXPECT_FALSE(context().libraryManager.removeLibrary(removed->id));
    QCoreApplication::processEvents();
    EXPECT_TRUE(libraryRemoved.isEmpty());
    EXPECT_TRUE(activation.isEmpty());
    EXPECT_TRUE(context().libraryManager.hasLibrary(removed->id));
    EXPECT_TRUE(context().settings.fileContains(removedInactiveKey));
    EXPECT_TRUE(context().settings.fileContains(removedStatsKey));
    expectStored(listedTrack, removed->id, Track::Stat::Loved);
    expectStored(detachedTrack, -1, Track::Stat::Loved);
    expectStored(keptTrack, kept->id, Track::Stat::Loved);
    expectInMemory(context().library, listedTrack, removed->id, Track::Stat::Loved);
    expectInMemory(context().library, detachedTrack, -1, Track::Stat::Loved);
    expectInMemory(context().library, keptTrack, kept->id, Track::Stat::Loved);
    EXPECT_FALSE(context().library.isTrackSourceActive(context().library.trackForId(listedTrack.id())));
    {
        const LibraryManager restarted{context().dbPool, &context().settings};
        ASSERT_TRUE(restarted.libraryInfo(removed->id));
        EXPECT_EQ(restarted.libraryInfo(removed->id)->path, removedPath);
        ASSERT_TRUE(restarted.libraryInfo(kept->id));
    }
    DbQuery dropUpdate{dbProvider.db(), u"DROP TRIGGER FailExtraProperties;"_s};
    ASSERT_TRUE(dropUpdate.exec()) << dropUpdate.lastError().text().toStdString();

    ASSERT_TRUE(context().libraryManager.removeLibrary(removed->id));
    EXPECT_EQ(libraryRemoved.size(), 1);
    EXPECT_EQ(activation.size(), 1);
    EXPECT_FALSE(context().libraryManager.hasLibrary(removed->id));
    EXPECT_FALSE(context().settings.fileContains(removedInactiveKey));
    EXPECT_FALSE(context().settings.fileContains(removedStatsKey));
    EXPECT_TRUE(context().settings.fileContains(keptStatsKey));
    expectStored(listedTrack, -1, Track::Stat::None);
    expectStored(detachedTrack, -1, Track::Stat::None);
    expectStored(keptTrack, kept->id, Track::Stat::Loved);
    expectInMemory(context().library, listedTrack, -1, Track::Stat::None);
    expectInMemory(context().library, detachedTrack, -1, Track::Stat::None);
    expectInMemory(context().library, keptTrack, kept->id, Track::Stat::Loved);
    UnifiedMusicLibrary reloaded{&context().libraryManager, context().dbPool,   context().playlistLoader,
                                 context().audioLoader,     context().remoteIo, &context().settings};
    QSignalSpy loaded{&reloaded, &MusicLibrary::tracksLoaded};
    reloaded.loadAllTracks();
    waitForSignal(loaded);
    expectInMemory(reloaded, listedTrack, -1, Track::Stat::None);
    expectInMemory(reloaded, detachedTrack, -1, Track::Stat::None);
    expectInMemory(reloaded, keptTrack, kept->id, Track::Stat::Loved);
}

TEST_F(UnifiedMusicLibraryTest, ExternalPlaylistOwnershipSurvivesUnlockAndSourceVisibilityChanges)
{
    const auto library = context().library.addExternalLibrary(u"test-source://account"_s, u"Account"_s);
    ASSERT_TRUE(library);
    QSignalSpy applied{&context().library, &MusicLibrary::externalLibrarySnapshotFinished};
    context().library.applyExternalLibrarySnapshot(
        *library, {externalTrack(u"test-source://account/item"_s, u"Title"_s)}, QUuid::createUuid());
    ASSERT_TRUE(waitForSignal(applied).at(2).toBool());
    const Track track = context().library.libraryTracks().front();
    Playlist* mirror  = context().playlistHandler.applyExternalPlaylist(
        library->path, u"server-playlist"_s, {}, u"Server playlist"_s, PlaylistTrack::fromTracks({track, track}, {}));
    ASSERT_NE(mirror, nullptr);
    const UId mirrorId = mirror->id();
    Playlist* local    = context().playlistHandler.createNewPlaylist(u"Local playlist"_s, {track});
    ASSERT_NE(local, nullptr);
    const UId localId = local->id();
    context().playlistHandler.setPlaylistLocked(mirrorId, false);
    EXPECT_TRUE(mirror->isLocked());
    context().playlistHandler.replacePlaylistTracks(mirrorId, TrackList{});
    context().playlistHandler.removePlaylistTracks(mirrorId, {0});
    context().playlistHandler.renamePlaylist(mirrorId, u"Changed"_s);
    context().playlistHandler.removePlaylist(mirrorId);
    EXPECT_EQ(mirror->trackCount(), 2);
    EXPECT_EQ(mirror->name(), u"Server playlist"_s);
    EXPECT_EQ(context().playlistHandler.playlistById(mirrorId), mirror);
    EXPECT_EQ(context().playlistHandler.applyExternalPlaylist(library->path, u"server-playlist"_s, {}, u"Refreshed"_s,
                                                              PlaylistTrack::fromTracks({track}, {})),
              mirror);
    EXPECT_EQ(mirror->trackCount(), 1);

    context().library.setExternalLibraryActive(*library, false);
    const auto visible = context().playlistHandler.playlists();
    EXPECT_EQ(std::ranges::find(visible, mirror), visible.end());
    EXPECT_NE(std::ranges::find(visible, local), visible.end());
    EXPECT_EQ(context().playlistHandler.playlistById(mirrorId), mirror);
    EXPECT_EQ(context().playlistHandler.playlistById(localId)->tracks().front().id(), track.id());
    context().library.setExternalLibraryActive(*library, true);
    const auto visibleAfterActivation = context().playlistHandler.playlists();
    EXPECT_NE(std::ranges::find(visibleAfterActivation, mirror), visibleAfterActivation.end());
    EXPECT_EQ(context().playlistHandler.playlistById(mirrorId), mirror);
    context().library.setExternalLibraryActive(*library, false);
    EXPECT_EQ(context().playlistHandler.applyExternalPlaylist(library->path, u"server-playlist"_s, {},
                                                              u"Hidden update"_s,
                                                              PlaylistTrack::fromTracks({track, track}, {})),
              mirror);
    EXPECT_FALSE(context().playlistHandler.isPlaylistVisible(mirrorId));
    EXPECT_EQ(mirror->trackCount(), 2);
    context().playlistHandler.savePlaylists();
    const PlaylistTrackList savedEntries = mirror->playlistTracks();
    {
        PlaylistHandler restored{context().dbPool, context().audioLoader, &context().library, &context().settings};
        Playlist* retained = restored.playlistByDbId(mirror->dbId());
        ASSERT_NE(retained, nullptr);
        EXPECT_EQ(retained->sourceLibraryPath(), library->path);
        EXPECT_TRUE(retained->isLocked());
        EXPECT_FALSE(restored.isPlaylistVisible(retained->id()));
        restored.setPlaylistLocked(retained->id(), false);
        EXPECT_TRUE(retained->isLocked());
        EXPECT_NE(restored.playlistByDbId(local->dbId()), nullptr);
        QSignalSpy loaded{&context().library, &MusicLibrary::tracksLoaded};
        context().library.loadAllTracks();
        waitForSignal(loaded);
        const auto restoredEntries = retained->playlistTracks();
        ASSERT_EQ(restoredEntries.size(), savedEntries.size());
        for(size_t index{0}; index < savedEntries.size(); ++index) {
            EXPECT_EQ(restoredEntries.at(index).track.id(), savedEntries.at(index).track.id());
            EXPECT_EQ(restoredEntries.at(index).entryId, savedEntries.at(index).entryId);
        }
    }
    context().playlistHandler.removeExternalPlaylist(library->path, mirrorId);
    EXPECT_EQ(context().playlistHandler.playlistById(mirrorId), nullptr);
    EXPECT_NE(context().playlistHandler.playlistById(localId), nullptr);
    const auto removed = context().playlistHandler.removedPlaylists();
    EXPECT_TRUE(
        std::ranges::none_of(removed, [mirrorId](const Playlist* playlist) { return playlist->id() == mirrorId; }));
}

TEST_F(UnifiedMusicLibraryTest, ExternalPlaylistWriteFailuresKeepStoredPlaylistsForRetry)
{
    const auto library = context().library.addExternalLibrary(u"test-source://failing"_s, u"Account"_s);
    ASSERT_TRUE(library);
    QSignalSpy applied{&context().library, &MusicLibrary::externalLibrarySnapshotFinished};
    context().library.applyExternalLibrarySnapshot(
        *library, {externalTrack(u"test-source://failing/item"_s, u"Title"_s)}, QUuid::createUuid());
    ASSERT_TRUE(waitForSignal(applied).at(2).toBool());
    const Track track = context().library.libraryTracks().front();
    Playlist* mirror  = context().playlistHandler.applyExternalPlaylist(
        library->path, u"server-playlist"_s, {}, u"Server playlist"_s, PlaylistTrack::fromTracks({track}, {}));
    ASSERT_NE(mirror, nullptr);
    const UId mirrorId             = mirror->id();
    // Loading the shared library would repopulate the live playlists from storage.
    UnifiedMusicLibrary storedLibrary{&context().libraryManager, context().dbPool,   context().playlistLoader,
                                      context().audioLoader,     context().remoteIo, &context().settings};
    const int mirrorDbId           = mirror->dbId();
    const size_t playlistCount     = context().playlistHandler.playlists().size();
    const auto sourcePlaylistDbIds = [&library](const PlaylistHandler& handler) {
        std::vector<int> dbIds;
        for(const Playlist* playlist : handler.playlists()) {
            if(playlist->sourceLibraryPath() == library->path) {
                dbIds.push_back(playlist->dbId());
            }
        }
        return dbIds;
    };

    const Fooyin::DbConnectionProvider dbProvider{context().dbPool};
    Fooyin::DbQuery failInsert{dbProvider.db(), u"CREATE TRIGGER FailPlaylistTracks BEFORE INSERT ON PlaylistTracks "
                                                u"BEGIN SELECT RAISE(ABORT, 'synthetic failure'); END;"_s};
    ASSERT_TRUE(failInsert.exec()) << failInsert.lastError().text().toStdString();
    Fooyin::DbQuery failDelete{dbProvider.db(), u"CREATE TRIGGER FailPlaylists BEFORE DELETE ON Playlists "
                                                u"BEGIN SELECT RAISE(ABORT, 'synthetic failure'); END;"_s};
    ASSERT_TRUE(failDelete.exec()) << failDelete.lastError().text().toStdString();

    QSignalSpy added{&context().playlistHandler, &PlaylistHandler::playlistAdded};
    QSignalSpy patched{&context().playlistHandler, &PlaylistHandler::tracksPatched};
    QSignalSpy changed{&context().playlistHandler, &PlaylistHandler::tracksChanged};
    EXPECT_EQ(context().playlistHandler.applyExternalPlaylist(library->path, u"new-playlist"_s, {}, u"New playlist"_s,
                                                              PlaylistTrack::fromTracks({track}, {})),
              nullptr);
    EXPECT_EQ(context().playlistHandler.playlists().size(), playlistCount);
    EXPECT_TRUE(added.isEmpty());
    EXPECT_TRUE(patched.isEmpty());
    EXPECT_TRUE(changed.isEmpty());
    QSignalSpy renamed{&context().playlistHandler, &PlaylistHandler::playlistRenamed};
    EXPECT_EQ(context().playlistHandler.applyExternalPlaylist(library->path, u"server-playlist"_s, {}, u"Renamed"_s,
                                                              PlaylistTrack::fromTracks({track, track}, {})),
              nullptr);
    EXPECT_EQ(mirror->name(), u"Renamed"_s);
    EXPECT_EQ(mirror->trackCount(), 2);
    EXPECT_TRUE(mirror->modified());
    EXPECT_EQ(renamed.size(), 1);
    EXPECT_FALSE(context().playlistHandler.removeExternalPlaylist(library->path, mirrorId));
    EXPECT_EQ(context().playlistHandler.playlistById(mirrorId), mirror);

    Fooyin::DbQuery dropInsert{dbProvider.db(), u"DROP TRIGGER FailPlaylistTracks;"_s};
    ASSERT_TRUE(dropInsert.exec()) << dropInsert.lastError().text().toStdString();
    Fooyin::DbQuery dropDelete{dbProvider.db(), u"DROP TRIGGER FailPlaylists;"_s};
    ASSERT_TRUE(dropDelete.exec()) << dropDelete.lastError().text().toStdString();
    {
        PlaylistHandler restored{context().dbPool, context().audioLoader, &storedLibrary, &context().settings};
        QSignalSpy loaded{&storedLibrary, &MusicLibrary::tracksLoaded};
        storedLibrary.loadAllTracks();
        waitForSignal(loaded);
        EXPECT_EQ(sourcePlaylistDbIds(restored), std::vector<int>{mirrorDbId});
        ASSERT_NE(restored.playlistByDbId(mirrorDbId), nullptr);
        EXPECT_EQ(restored.playlistByDbId(mirrorDbId)->name(), u"Server playlist"_s);
        EXPECT_EQ(restored.playlistByDbId(mirrorDbId)->trackCount(), 1);
    }

    EXPECT_EQ(context().playlistHandler.applyExternalPlaylist(library->path, u"server-playlist"_s, {}, u"Renamed"_s,
                                                              PlaylistTrack::fromTracks({track, track}, {})),
              mirror);
    EXPECT_EQ(mirror->name(), u"Renamed"_s);
    {
        PlaylistHandler restored{context().dbPool, context().audioLoader, &storedLibrary, &context().settings};
        QSignalSpy loaded{&storedLibrary, &MusicLibrary::tracksLoaded};
        storedLibrary.loadAllTracks();
        waitForSignal(loaded);
        EXPECT_EQ(sourcePlaylistDbIds(restored), std::vector<int>{mirrorDbId});
        ASSERT_NE(restored.playlistByDbId(mirrorDbId), nullptr);
        EXPECT_EQ(restored.playlistByDbId(mirrorDbId)->name(), u"Renamed"_s);
        EXPECT_EQ(restored.playlistByDbId(mirrorDbId)->trackCount(), 2);
    }
    EXPECT_TRUE(context().playlistHandler.removeExternalPlaylist(library->path, mirrorId));
    EXPECT_EQ(context().playlistHandler.playlistById(mirrorId), nullptr);
}

TEST_F(UnifiedMusicLibraryTest, ExternalPlaylistOuterCommitFailureKeepsChangesForExactRetry)
{
    const auto library = context().library.addExternalLibrary(u"test-source://commit-failure"_s, u"Account"_s);
    ASSERT_TRUE(library);
    QSignalSpy applied{&context().library, &MusicLibrary::externalLibrarySnapshotFinished};
    context().library.applyExternalLibrarySnapshot(
        *library,
        {externalTrack(u"test-source://commit-failure/first"_s, u"First"_s),
         externalTrack(u"test-source://commit-failure/second"_s, u"Second"_s)},
        QUuid::createUuid());
    ASSERT_TRUE(waitForSignal(applied).at(2).toBool());
    const TrackList tracks = context().library.libraryTracks();
    ASSERT_EQ(tracks.size(), 2U);
    const UId firstEntryId{QUuid::createUuid()};
    const UId secondEntryId{QUuid::createUuid()};
    Playlist* mirror = context().playlistHandler.applyExternalPlaylist(
        library->path, u"server-playlist"_s, {}, u"Server playlist"_s,
        {{.track = tracks.at(0), .playlistId = {}, .entryId = firstEntryId, .indexInPlaylist = 0}});
    ASSERT_NE(mirror, nullptr);
    const int mirrorDbId = mirror->dbId();
    // Loading the shared library would repopulate the live playlists from storage.
    UnifiedMusicLibrary storedLibrary{&context().libraryManager, context().dbPool,   context().playlistLoader,
                                      context().audioLoader,     context().remoteIo, &context().settings};
    const PlaylistTrackList updatedTracks{
        {.track = tracks.at(1), .playlistId = {}, .entryId = secondEntryId, .indexInPlaylist = 0},
        {.track = tracks.at(0), .playlistId = {}, .entryId = firstEntryId, .indexInPlaylist = 1}};

    const Fooyin::DbConnectionProvider dbProvider{context().dbPool};
    const auto execute = [&dbProvider](const QString& statement) {
        Fooyin::DbQuery query{dbProvider.db(), statement};
        const bool executed = query.exec();
        EXPECT_TRUE(executed) << query.lastError().text().toStdString();
        return executed;
    };
    // A deferred foreign key passes the nested savepoint and fails only the outermost release.
    ASSERT_TRUE(execute(u"CREATE TABLE CommitFaultParent (ID INTEGER PRIMARY KEY);"_s));
    ASSERT_TRUE(execute(u"CREATE TABLE CommitFault (ParentID INTEGER REFERENCES CommitFaultParent(ID) "
                        u"DEFERRABLE INITIALLY DEFERRED);"_s));
    ASSERT_TRUE(execute(u"CREATE TRIGGER FailCommit AFTER UPDATE ON Playlists "
                        u"BEGIN INSERT INTO CommitFault VALUES (1); END;"_s));

    const auto sourcePlaylistCount = [](const PlaylistHandler& handler) {
        return std::ranges::count_if(handler.playlists(), [](const Playlist* playlist) {
            return playlist->sourcePlaylistId() == u"server-playlist"_s;
        });
    };
    const auto expectVisibleTracks = [&](const Playlist* playlist) {
        const PlaylistTrackList visibleTracks = playlist->playlistTracks();
        ASSERT_EQ(visibleTracks.size(), 2U);
        EXPECT_EQ(visibleTracks.at(0).track.id(), tracks.at(1).id());
        EXPECT_EQ(visibleTracks.at(0).entryId, secondEntryId);
        EXPECT_EQ(visibleTracks.at(1).track.id(), tracks.at(0).id());
        EXPECT_EQ(visibleTracks.at(1).entryId, firstEntryId);
    };

    QSignalSpy patched{&context().playlistHandler, &PlaylistHandler::tracksPatched};
    QSignalSpy changed{&context().playlistHandler, &PlaylistHandler::tracksChanged};
    EXPECT_EQ(context().playlistHandler.applyExternalPlaylist(library->path, u"server-playlist"_s, {},
                                                              u"Server playlist"_s, updatedTracks),
              nullptr);
    expectVisibleTracks(mirror);
    EXPECT_EQ(patched.size() + changed.size(), 1);
    EXPECT_TRUE(mirror->modified());
    EXPECT_TRUE(mirror->tracksModified());
    EXPECT_EQ(mirror->sourcePlaylistId(), u"server-playlist"_s);
    EXPECT_EQ(sourcePlaylistCount(context().playlistHandler), 1);
    const QString pendingEntries = mirror->extraProperties().value(u"core/source-playlist-entries"_s);
    {
        PlaylistHandler restored{context().dbPool, context().audioLoader, &storedLibrary, &context().settings};
        QSignalSpy loaded{&storedLibrary, &MusicLibrary::tracksLoaded};
        storedLibrary.loadAllTracks();
        waitForSignal(loaded);
        ASSERT_NE(restored.playlistByDbId(mirrorDbId), nullptr);
        EXPECT_EQ(restored.playlistByDbId(mirrorDbId)->trackCount(), 1);
    }

    ASSERT_TRUE(execute(u"DROP TRIGGER FailCommit;"_s));
    ASSERT_TRUE(execute(u"DROP TABLE CommitFault;"_s));
    ASSERT_TRUE(execute(u"DROP TABLE CommitFaultParent;"_s));
    context().playlistHandler.savePlaylists();
    {
        PlaylistHandler restored{context().dbPool, context().audioLoader, &storedLibrary, &context().settings};
        QSignalSpy loaded{&storedLibrary, &MusicLibrary::tracksLoaded};
        storedLibrary.loadAllTracks();
        waitForSignal(loaded);
        const Playlist* stored = restored.playlistByDbId(mirrorDbId);
        ASSERT_NE(stored, nullptr);
        expectVisibleTracks(stored);
        EXPECT_EQ(stored->extraProperties().value(u"core/source-playlist-entries"_s), pendingEntries);
        EXPECT_EQ(sourcePlaylistCount(restored), 1);
    }

    EXPECT_EQ(context().playlistHandler.applyExternalPlaylist(library->path, u"server-playlist"_s, {},
                                                              u"Server playlist"_s, updatedTracks),
              mirror);
    EXPECT_EQ(sourcePlaylistCount(context().playlistHandler), 1);
    {
        PlaylistHandler restored{context().dbPool, context().audioLoader, &storedLibrary, &context().settings};
        QSignalSpy loaded{&storedLibrary, &MusicLibrary::tracksLoaded};
        storedLibrary.loadAllTracks();
        waitForSignal(loaded);
        const Playlist* stored = restored.playlistByDbId(mirrorDbId);
        ASSERT_NE(stored, nullptr);
        EXPECT_EQ(stored->name(), u"Server playlist"_s);
        EXPECT_EQ(stored->sourcePlaylistId(), u"server-playlist"_s);
        EXPECT_EQ(stored->extraProperties().value(u"core/source-playlist-entries"_s),
                  mirror->extraProperties().value(u"core/source-playlist-entries"_s));
        const PlaylistTrackList storedTracks = stored->playlistTracks();
        ASSERT_EQ(storedTracks.size(), 2U);
        EXPECT_EQ(storedTracks.at(0).track.id(), tracks.at(1).id());
        EXPECT_EQ(storedTracks.at(0).entryId, secondEntryId);
        EXPECT_EQ(storedTracks.at(1).track.id(), tracks.at(0).id());
        EXPECT_EQ(storedTracks.at(1).entryId, firstEntryId);
    }
}

TEST_F(UnifiedMusicLibraryTest, ExternalPlaylistIdentityAdoptsOnlyOwnedPlaylistsWithoutSourcePlaylistId)
{
    const auto library = context().library.addExternalLibrary(u"test-source://identity"_s, u"Account"_s);
    const auto other   = context().library.addExternalLibrary(u"test-source://other-identity"_s, u"Other"_s);
    ASSERT_TRUE(library);
    ASSERT_TRUE(other);
    PlaylistHandler& handler = context().playlistHandler;
    Playlist* foreign        = handler.applyExternalPlaylist(other->path, u"foreign"_s, {}, u"Foreign"_s, {});
    Playlist* local          = handler.createPlaylist(u"Local"_s, {});
    ASSERT_NE(foreign, nullptr);
    ASSERT_NE(local, nullptr);

    Playlist* foreignAdoption = handler.applyExternalPlaylist(library->path, u"first"_s, foreign->id(), u"First"_s, {});
    ASSERT_NE(foreignAdoption, nullptr);
    EXPECT_NE(foreignAdoption, foreign);
    EXPECT_EQ(foreign->sourceLibraryPath(), other->path);
    EXPECT_NE(handler.applyExternalPlaylist(library->path, u"second"_s, local->id(), u"Second"_s, {}), local);
    EXPECT_TRUE(local->sourceLibraryPath().isEmpty());

    EXPECT_EQ(handler.applyExternalPlaylist(library->path, u"first"_s, {}, u"First renamed"_s, {}), foreignAdoption);
    EXPECT_NE(handler.applyExternalPlaylist(library->path, u"third"_s, foreignAdoption->id(), u"Third"_s, {}),
              foreignAdoption);
    EXPECT_EQ(foreignAdoption->sourcePlaylistId(), u"first"_s);
    EXPECT_EQ(handler.applyExternalPlaylist(library->path, {}, {}, u"Missing"_s, {}), nullptr);
    EXPECT_EQ(handler.externalPlaylists(library->path).size(), 3U);

    Playlist* third = handler.playlistByName(u"Third"_s);
    ASSERT_NE(third, nullptr);
    third->setExtraProperty(u"core/source-playlist-id"_s, u"first"_s);
    EXPECT_EQ(handler.applyExternalPlaylist(library->path, u"first"_s, {}, u"Ambiguous"_s, {}), nullptr);
    EXPECT_EQ(foreignAdoption->name(), u"First renamed"_s);
    EXPECT_EQ(third->name(), u"Third"_s);
}

TEST_F(UnifiedMusicLibraryTest, ExternalSnapshotAcknowledgesEachExactOperationOnce)
{
    const auto library = context().library.addExternalLibrary(u"test-source://correlated"_s, u"External"_s);
    ASSERT_TRUE(library);
    const QUuid accepted = QUuid::createUuid();
    const QUuid rejected = QUuid::createUuid();
    QSignalSpy finished{&context().library, &MusicLibrary::externalLibrarySnapshotFinished};
    context().library.applyExternalLibrarySnapshot(
        *library, {externalTrack(u"test-source://correlated/item"_s, u"Title"_s)}, accepted);
    context().library.applyExternalLibrarySnapshot(
        *library, {externalTrack(u"test-source://foreign/item"_s, u"Rejected"_s)}, rejected);
    context().library.applyExternalLibrarySnapshot(*library, {}, {});
    ASSERT_TRUE(waitForCondition([&]() { return finished.size() == 3; }));
    EXPECT_EQ(finished.at(0).at(0).toUuid(), accepted);
    EXPECT_EQ(finished.at(0).at(1).toInt(), library->id);
    EXPECT_TRUE(finished.at(0).at(2).toBool());
    EXPECT_EQ(finished.at(1).at(0).toUuid(), rejected);
    EXPECT_FALSE(finished.at(1).at(2).toBool());
    EXPECT_TRUE(finished.at(2).at(0).toUuid().isNull());
    EXPECT_FALSE(finished.at(2).at(2).toBool());
    QCoreApplication::processEvents();
    EXPECT_EQ(finished.size(), 3);
    ASSERT_EQ(context().library.libraryTracks().size(), 1U);
    EXPECT_EQ(context().library.libraryTracks().front().title(), u"Title"_s);
}

TEST_F(UnifiedMusicLibraryTest, SnapshotPreservesPersistedRatingAndRejectsDelayedOldLovedPublication)
{
    const auto library = context().library.addExternalLibrary(u"test-source://stats-race"_s, u"External"_s);
    ASSERT_TRUE(library);
    context().library.setExternalLibraryReadOnlyStats(*library, Track::Stat::Loved);
    Track imported = externalTrack(u"test-source://stats-race/item"_s, u"Initial"_s);
    QSignalSpy applied{&context().library, &MusicLibrary::externalLibrarySnapshotFinished};
    context().library.applyExternalLibrarySnapshot(*library, {imported}, QUuid::createUuid());
    ASSERT_TRUE(waitForSignal(applied).at(2).toBool());
    const Track initial = context().library.libraryTracks().front();
    Track rated{initial};
    rated.setRating(0.8F);
    auto* handler = dynamic_cast<QObject*>(context().library.pendingTrackCoverProvider());
    ASSERT_NE(handler, nullptr);
    QSignalBlocker delayedCompletion{handler};
    context().library.updateTrackStats(rated, Track::Stat::Rating);
    TrackDatabase database;
    database.initialise(DbConnectionProvider{context().dbPool});
    Track committed{initial};
    ASSERT_TRUE(waitForCondition([&]() { return database.reloadTrack(committed) && committed.rating() == 0.8F; }));
    EXPECT_FALSE(committed.isLoved());
    EXPECT_EQ(context().library.trackForId(initial.id()).rating(), initial.rating());
    LibrarySortBarrier publication;
    imported.setTitle(u"Refreshed"_s);
    imported.setLoved(true);
    context().library.applyExternalLibrarySnapshot(*library, {imported}, QUuid::createUuid());
    Track snapshot{initial};
    ASSERT_TRUE(waitForCondition([&]() { return database.reloadTrack(snapshot) && snapshot.isLoved(); }));
    EXPECT_EQ(snapshot.rating(), 0.8F);
    EXPECT_TRUE(applied.isEmpty());
    delayedCompletion.unblock();
    ASSERT_TRUE(QMetaObject::invokeMethod(handler, "tracksStatsUpdated", Qt::DirectConnection,
                                          Q_ARG(Fooyin::TrackList, TrackList{committed}),
                                          Q_ARG(Fooyin::Track::Stats, Track::Stats{Track::Stat::Rating})));
    EXPECT_FALSE(context().library.trackForId(initial.id()).isLoved());
    publication.release();
    ASSERT_TRUE(waitForSignal(applied).at(2).toBool());
    ASSERT_TRUE(waitForCondition([&]() {
        const Track resident = context().library.trackForId(initial.id());
        return resident.rating() == 0.8F && resident.isLoved();
    }));
    QCoreApplication::processEvents();
    EXPECT_TRUE(context().library.trackForId(initial.id()).isLoved());
    UnifiedMusicLibrary reconstructed{&context().libraryManager, context().dbPool,   context().playlistLoader,
                                      context().audioLoader,     context().remoteIo, &context().settings};
    QSignalSpy loaded{&reconstructed, &MusicLibrary::tracksLoaded};
    reconstructed.loadAllTracks();
    waitForSignal(loaded);
    EXPECT_EQ(reconstructed.trackForId(initial.id()).rating(), 0.8F);
    EXPECT_TRUE(reconstructed.trackForId(initial.id()).isLoved());
}

TEST_F(UnifiedMusicLibraryTest, RatingCommittedDuringSnapshotSortPublishesAfterSnapshotWithoutRollback)
{
    const auto library = context().library.addExternalLibrary(u"test-source://stats-order"_s, u"External"_s);
    ASSERT_TRUE(library);
    context().library.setExternalLibraryReadOnlyStats(*library, Track::Stat::Loved);
    Track imported = externalTrack(u"test-source://stats-order/item"_s, u"Initial"_s);
    QSignalSpy applied{&context().library, &MusicLibrary::externalLibrarySnapshotFinished};
    context().library.applyExternalLibrarySnapshot(*library, {imported}, QUuid::createUuid());
    ASSERT_TRUE(waitForSignal(applied).at(2).toBool());
    const Track initial = context().library.libraryTracks().front();
    auto* handler       = dynamic_cast<QObject*>(context().library.pendingTrackCoverProvider());
    ASSERT_NE(handler, nullptr);
    QSignalSpy completed{handler, SIGNAL(tracksStatsUpdated(Fooyin::TrackList, Fooyin::Track::Stats))};
    ASSERT_TRUE(completed.isValid());
    LibrarySortBarrier publication;
    imported.setTitle(u"Refreshed"_s);
    imported.setLoved(true);
    context().library.applyExternalLibrarySnapshot(*library, {imported}, QUuid::createUuid());
    TrackDatabase database;
    database.initialise(DbConnectionProvider{context().dbPool});
    Track stored{initial};
    ASSERT_TRUE(waitForCondition([&]() { return database.reloadTrack(stored) && stored.isLoved(); }));
    EXPECT_EQ(stored.rating(), initial.rating());
    Track rated{initial};
    rated.setRating(0.6F);
    context().library.updateTrackStats(rated, Track::Stat::Rating);
    ASSERT_TRUE(waitForCondition([&]() { return !completed.isEmpty(); }));
    ASSERT_TRUE(database.reloadTrack(stored));
    EXPECT_EQ(stored.rating(), 0.6F);
    EXPECT_TRUE(stored.isLoved());
    EXPECT_TRUE(applied.isEmpty());
    EXPECT_EQ(context().library.trackForId(initial.id()).rating(), initial.rating());
    publication.release();
    ASSERT_TRUE(waitForSignal(applied).at(2).toBool());
    ASSERT_TRUE(waitForCondition([&]() { return context().library.trackForId(initial.id()).rating() == 0.6F; }));
    EXPECT_TRUE(context().library.trackForId(initial.id()).isLoved());
    UnifiedMusicLibrary reconstructed{&context().libraryManager, context().dbPool,   context().playlistLoader,
                                      context().audioLoader,     context().remoteIo, &context().settings};
    QSignalSpy loaded{&reconstructed, &MusicLibrary::tracksLoaded};
    reconstructed.loadAllTracks();
    waitForSignal(loaded);
    EXPECT_EQ(reconstructed.trackForId(initial.id()).rating(), 0.6F);
    EXPECT_TRUE(reconstructed.trackForId(initial.id()).isLoved());
}

TEST_F(UnifiedMusicLibraryTest, HiddenSourcePlaylistCannotBeActivatedOrSelectedThroughRetainedPlaybackReferences)
{
    const QByteArray previousStateDirectory = qgetenv("XDG_STATE_HOME");
    qputenv("XDG_STATE_HOME", context().tempDir.filePath(u"state"_s).toUtf8());
    const auto restoreStateDirectory = qScopeGuard([&]() {
        if(previousStateDirectory.isNull()) {
            qunsetenv("XDG_STATE_HOME");
        }
        else {
            qputenv("XDG_STATE_HOME", previousStateDirectory);
        }
    });
    const auto source                = context().library.addExternalLibrary(u"test-source://playback"_s, u"External"_s);
    ASSERT_TRUE(source);
    QSignalSpy applied{&context().library, &MusicLibrary::externalLibrarySnapshotFinished};
    context().library.applyExternalLibrarySnapshot(
        *source, {externalTrack(u"test-source://playback/item"_s, u"Title"_s)}, QUuid::createUuid());
    ASSERT_TRUE(waitForSignal(applied).at(2).toBool());
    const Track track = context().library.libraryTracks().front();
    Playlist* mirror  = context().playlistHandler.applyExternalPlaylist(source->path, u"owned"_s, {}, u"Owned"_s,
                                                                        PlaylistTrack::fromTracks({track}, {}));
    ASSERT_NE(mirror, nullptr);
    Playlist* local = context().playlistHandler.createNewPlaylist(u"Local references"_s, {track});
    ASSERT_NE(local, nullptr);
    PlayerController player{&context().settings, &context().playlistHandler};
    context().playlistHandler.changeActivePlaylist(mirror);
    const auto retained = mirror->playlistTrack(0);
    ASSERT_TRUE(retained);
    player.commitCurrentTrack(*retained);
    context().library.setExternalLibraryActive(*source, false);
    EXPECT_EQ(context().playlistHandler.activePlaylist(), nullptr);
    QSignalSpy active{&context().playlistHandler, &PlaylistHandler::activePlaylistChanged};
    QSignalSpy requested{&player, &PlayerController::trackChangeRequested};
    context().playlistHandler.changeActivePlaylist(mirror->id());
    context().playlistHandler.changeActivePlaylist(mirror);
    player.startPlayback(mirror->id());
    player.startPlayback(mirror);
    player.startPlayback(mirror, {*retained});
    player.changeCurrentTrack(*retained, {.reason = Player::AdvanceReason::StartupRestore});
    player.play();
    player.next();
    player.previous();
    EXPECT_TRUE(active.isEmpty());
    EXPECT_TRUE(requested.isEmpty());
    EXPECT_EQ(context().playlistHandler.activePlaylist(), nullptr);
    EXPECT_EQ(context().playlistHandler.playlistById(mirror->id()), mirror);
    player.startPlayback(local);
    ASSERT_EQ(requested.size(), 1);
    EXPECT_EQ(requested.front().front().value<Player::TrackChangeRequest>().track.playlistId, local->id());
    player.reset();
    requested.clear();
    context().library.setExternalLibraryActive(*source, true);
    player.startPlayback(mirror);
    ASSERT_EQ(requested.size(), 1);
    EXPECT_EQ(context().playlistHandler.activePlaylist(), mirror);
}

TEST_F(UnifiedMusicLibraryTest, SourcePlaylistStoredOrderAndVisibleProjectionSurviveReconstruction)
{
    const auto source = context().library.addExternalLibrary(u"test-source://playlist-order"_s, u"Order"_s);
    ASSERT_TRUE(source);
    auto& handler                = context().playlistHandler;
    const auto existingPlaylists = handler.playlists();
    auto* z2                     = handler.createNewPlaylist(u"Z2"_s);
    for(const Playlist* playlist : existingPlaylists) {
        handler.removePlaylist(playlist->id());
    }
    auto* z = handler.createNewPlaylist(u"Z"_s);
    auto* a = handler.applyExternalPlaylist(source->path, u"a"_s, {}, u"A"_s, {});
    auto* b = handler.applyExternalPlaylist(source->path, u"b"_s, {}, u"B"_s, {});
    auto* c = handler.applyExternalPlaylist(source->path, u"c"_s, {}, u"C"_s, {});
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    ASSERT_NE(c, nullptr);
    context().library.setExternalLibraryActive(*source, false);
    ASSERT_TRUE(handler.sortPlaylistsByName(Qt::AscendingOrder));
    EXPECT_EQ(a->index(), 0);
    EXPECT_EQ(b->index(), 1);
    EXPECT_EQ(c->index(), 2);
    EXPECT_EQ(z->index(), 3);
    EXPECT_EQ(z2->index(), 4);
    handler.changePlaylistIndex(z->id(), 1);
    handler.changePlaylistIndex(z2->id(), 3);
    std::vector<int> storedDbIds;
    for(int index{0}; index < handler.playlistCount(); ++index) {
        storedDbIds.push_back(handler.playlistByIndex(index)->dbId());
    }
    std::vector<int> visibleDbIds;
    for(const Playlist* playlist : handler.playlists()) {
        visibleDbIds.push_back(playlist->dbId());
    }
    handler.savePlaylists();
    PlaylistHandler restored{context().dbPool, context().audioLoader, &context().library, &context().settings};
    ASSERT_EQ(restored.playlistCount(), storedDbIds.size());
    for(int index{0}; index < restored.playlistCount(); ++index) {
        EXPECT_EQ(restored.playlistByIndex(index)->dbId(), storedDbIds.at(index));
    }
    ASSERT_EQ(restored.playlists().size(), visibleDbIds.size());
    for(size_t row{0}; row < visibleDbIds.size(); ++row) {
        EXPECT_EQ(restored.playlists().at(row)->dbId(), visibleDbIds.at(row));
    }
    context().library.setExternalLibraryActive(*source, true);
    ASSERT_EQ(restored.playlists().size(), storedDbIds.size());
    for(size_t row{0}; row < storedDbIds.size(); ++row) {
        EXPECT_EQ(restored.playlists().at(row)->dbId(), storedDbIds.at(row));
    }
}

TEST_F(UnifiedMusicLibraryTest, SourceActivationInvalidatesVisibleCatalogBeforeHeldGlobalFilterCompletes)
{
    const auto a = context().library.addExternalLibrary(u"test-source://visible-a"_s, u"A"_s);
    const auto b = context().library.addExternalLibrary(u"test-source://visible-b"_s, u"B"_s);
    ASSERT_TRUE(a);
    ASSERT_TRUE(b);
    QSignalSpy applied{&context().library, &MusicLibrary::externalLibrarySnapshotFinished};
    context().library.applyExternalLibrarySnapshot(
        *a, {externalTrack(u"test-source://visible-a/item"_s, u"Shared title"_s)}, QUuid::createUuid());
    ASSERT_TRUE(waitForSignal(applied).at(2).toBool());
    context().library.applyExternalLibrarySnapshot(
        *b, {externalTrack(u"test-source://visible-b/item"_s, u"Shared title"_s)}, QUuid::createUuid());
    ASSERT_TRUE(waitForSignal(applied).at(2).toBool());
    context().library.setExternalLibraryActive(*b, false);
    context().library.setActiveLibraryFilters({{.id = 1, .name = u"Title"_s, .expression = u"title:Shared"_s}});
    ASSERT_TRUE(waitForCondition([&]() { return context().library.visibleLibraryTracks().size() == 1; }));
    LibrarySortBarrier background;
    context().library.setActiveLibraryFilters({{.id = 2, .name = u"Held title"_s, .expression = u"title:title"_s}});
    QSignalSpy changed{&context().library, &MusicLibrary::visibleLibraryTracksChanged};
    context().library.setExternalLibraryActive(*a, false);
    EXPECT_FALSE(changed.isEmpty());
    EXPECT_TRUE(context().library.visibleLibraryTracks().empty());
    context().library.setExternalLibraryActive(*b, true);
    const auto connection
        = QObject::connect(&context().library, &MusicLibrary::visibleLibraryTracksChanged, &context().library, [&]() {
              for(const Track& track : context().library.visibleLibraryTracks()) {
                  EXPECT_EQ(track.externalLibraryPath(), b->path);
              }
          });
    background.release();
    ASSERT_TRUE(waitForCondition([&]() { return context().library.visibleLibraryTracks().size() == 1; }));
    EXPECT_EQ(context().library.visibleLibraryTracks().front().externalLibraryPath(), b->path);
    EXPECT_EQ(context().library.libraryTracks().size(), 2U);
    QObject::disconnect(connection);
}

TEST_F(UnifiedMusicLibraryTest, ExternalStatsEditRequestsOnlyTheActiveHandlerOfTheOwningLibrary)
{
    const auto library = context().library.addExternalLibrary(u"test-source://favourites"_s, u"External"_s);
    const auto foreign = context().library.addExternalLibrary(u"test-source://foreign"_s, u"Foreign"_s);
    ASSERT_TRUE(library);
    ASSERT_TRUE(foreign);
    context().library.setExternalLibraryReadOnlyStats(*library, Track::Stat::Loved);
    context().library.setExternalLibraryReadOnlyStats(*foreign, Track::Stat::Loved);
    QSignalSpy applied{&context().library, &MusicLibrary::externalLibrarySnapshotFinished};
    context().library.applyExternalLibrarySnapshot(
        *library, {externalTrack(u"test-source://favourites/item"_s, u"Item"_s)}, QUuid::createUuid());
    ASSERT_TRUE(waitForSignal(applied).at(2).toBool());
    context().library.applyExternalLibrarySnapshot(
        *foreign, {externalTrack(u"test-source://foreign/item"_s, u"Foreign"_s)}, QUuid::createUuid());
    ASSERT_TRUE(waitForSignal(applied).at(2).toBool());
    const auto byPath        = tracksByPath(context().library.libraryTracks());
    const Track track        = byPath.at(u"test-source://favourites/item"_s);
    const Track foreignTrack = byPath.at(u"test-source://foreign/item"_s);

    std::vector<std::tuple<LibraryInfo, TrackList, Track::Stats>> requests;
    QObject::connect(&context().library, &MusicLibrary::externalTrackStatsChangeRequested, &context().library,
                     [&requests](const LibraryInfo& requestLibrary, const TrackList& tracks, Track::Stats stats) {
                         requests.emplace_back(requestLibrary, tracks, stats);
                     });
    QSignalSpy handlerChanged{&context().library, &MusicLibrary::externalLibraryStatsHandlerChanged};

    Track loved{track};
    loved.setLoved(true);
    Track foreignLoved{foreignTrack};
    foreignLoved.setLoved(true);

    EXPECT_FALSE(context().library.canEditTrackStats(track, Track::Stat::Loved));
    EXPECT_TRUE(context().library.canEditTrackStats(track, Track::Stat::Rating));
    context().library.editTrackStats({loved}, Track::Stat::Loved);
    QCoreApplication::processEvents();
    EXPECT_TRUE(requests.empty());

    auto handler = std::make_unique<QObject>();
    context().library.setExternalLibraryStatsHandler(*library, Track::Stat::Loved | Track::Stat::Rating, handler.get());
    EXPECT_EQ(handlerChanged.size(), 1);
    EXPECT_TRUE(context().library.canEditTrackStats(track, Track::Stat::Loved));
    EXPECT_TRUE(context().library.canEditTrackStats(track, Track::Stat::Loved | Track::Stat::Rating));
    EXPECT_FALSE(context().library.canEditTrackStats(foreignTrack, Track::Stat::Loved));

    context().library.updateTrackStats({loved}, Track::Stat::Loved);
    QCoreApplication::processEvents();
    EXPECT_TRUE(requests.empty());

    loved.setRating(0.6F);
    context().library.editTrackStats({loved, foreignLoved}, Track::Stat::Loved | Track::Stat::Rating);
    ASSERT_EQ(requests.size(), 1U);
    const auto& [requestLibrary, requestTracks, requestStats] = requests.front();
    EXPECT_EQ(requestLibrary.id, library->id);
    EXPECT_EQ(requestLibrary.path, library->path);
    EXPECT_EQ(requestStats, Track::Stats{Track::Stat::Loved});
    ASSERT_EQ(requestTracks.size(), 1U);
    EXPECT_EQ(requestTracks.front().id(), track.id());
    EXPECT_TRUE(requestTracks.front().isLoved());
    EXPECT_FLOAT_EQ(requestTracks.front().rating(), track.rating());
    ASSERT_TRUE(waitForCondition([&]() { return context().library.trackForId(track.id()).rating() == 0.6F; }));
    EXPECT_FALSE(context().library.trackForId(track.id()).isLoved());
    EXPECT_FALSE(context().library.trackForId(foreignTrack.id()).isLoved());
    TrackDatabase database;
    database.initialise(DbConnectionProvider{context().dbPool});
    Track stored{track};
    ASSERT_TRUE(database.reloadTrack(stored));
    EXPECT_FALSE(stored.isLoved());
    EXPECT_FLOAT_EQ(stored.rating(), 0.6F);
    requests.clear();

    context().library.setExternalLibraryActive(*library, false);
    EXPECT_FALSE(context().library.canEditTrackStats(track, Track::Stat::Loved));
    context().library.editTrackStats({loved}, Track::Stat::Loved);
    EXPECT_TRUE(requests.empty());
    context().library.setExternalLibraryActive(*library, true);
    EXPECT_TRUE(context().library.canEditTrackStats(track, Track::Stat::Loved));

    handler.reset();
    EXPECT_EQ(handlerChanged.size(), 2);
    EXPECT_FALSE(context().library.canEditTrackStats(track, Track::Stat::Loved));
    context().library.editTrackStats({loved}, Track::Stat::Loved);
    EXPECT_TRUE(requests.empty());

    auto replacement = std::make_unique<QObject>();
    context().library.setExternalLibraryStatsHandler(*library, Track::Stat::Loved, replacement.get());
    EXPECT_EQ(handlerChanged.size(), 3);
    context().library.setExternalLibraryStatsHandler(*library, Track::Stat::None, replacement.get());
    EXPECT_EQ(handlerChanged.size(), 4);
    EXPECT_FALSE(context().library.canEditTrackStats(track, Track::Stat::Loved));
    context().library.setExternalLibraryStatsHandler(*library, Track::Stat::Loved, replacement.get());
    EXPECT_EQ(handlerChanged.size(), 5);

    ASSERT_TRUE(context().libraryManager.removeLibrary(library->id));
    EXPECT_EQ(handlerChanged.size(), 6);
    ASSERT_TRUE(waitForCondition([&]() { return context().library.trackForId(track.id()).libraryId() == -1; }));
    EXPECT_TRUE(context().library.canEditTrackStats(context().library.trackForId(track.id()), Track::Stat::Loved));
    context().library.editTrackStats({loved}, Track::Stat::Loved);
    EXPECT_TRUE(requests.empty());
    context().library.setExternalLibraryStatsHandler(*library, Track::Stat::Loved, replacement.get());
    EXPECT_EQ(handlerChanged.size(), 6);
    replacement.reset();
    EXPECT_EQ(handlerChanged.size(), 6);
}

TEST_F(UnifiedMusicLibraryTest, ExternalStatsPublicationReplacesOnlySourceStatsInCommitOrder)
{
    const auto library = context().library.addExternalLibrary(u"test-source://published"_s, u"External"_s);
    const auto foreign = context().library.addExternalLibrary(u"test-source://other"_s, u"Foreign"_s);
    ASSERT_TRUE(library);
    ASSERT_TRUE(foreign);
    context().library.setExternalLibraryReadOnlyStats(*library, Track::Stat::Loved);
    context().library.setExternalLibraryReadOnlyStats(*foreign, Track::Stat::Loved);
    Track source = externalTrack(u"test-source://published/item"_s, u"Item"_s);
    QSignalSpy applied{&context().library, &MusicLibrary::externalLibrarySnapshotFinished};
    context().library.applyExternalLibrarySnapshot(*library, {source}, QUuid::createUuid());
    ASSERT_TRUE(waitForSignal(applied).at(2).toBool());
    context().library.applyExternalLibrarySnapshot(*foreign, {externalTrack(u"test-source://other/item"_s, u"Other"_s)},
                                                   QUuid::createUuid());
    ASSERT_TRUE(waitForSignal(applied).at(2).toBool());
    const auto byPath        = tracksByPath(context().library.libraryTracks());
    const Track track        = byPath.at(u"test-source://published/item"_s);
    const Track foreignTrack = byPath.at(u"test-source://other/item"_s);
    ASSERT_NE(context().playlistHandler.createPlaylist(u"Local"_s, {track}), nullptr);

    Track rated{track};
    rated.setRating(0.4F);
    context().library.updateTrackStats(rated, Track::Stat::Rating);
    ASSERT_TRUE(waitForCondition([&]() { return context().library.trackForId(track.id()).rating() == 0.4F; }));

    QSignalSpy requested{&context().library, &MusicLibrary::externalTrackStatsChangeRequested};
    QSignalSpy statsChanged{&context().library, &MusicLibrary::tracksStatsChanged};
    QSignalSpy finished{&context().library, &MusicLibrary::externalTrackStatsFinished};
    std::vector<QUuid> operations;
    auto publish = [&](const LibraryInfo& publishedLibrary, const TrackList& tracks, Track::Stats stats) {
        operations.push_back(QUuid::createUuid());
        context().library.applyExternalTrackStats(publishedLibrary, tracks, stats, operations.back());
    };
    auto waitForFinished = [&]() {
        return waitForCondition([&]() { return std::cmp_equal(finished.size(), operations.size()); });
    };

    Track reported{track};
    reported.setLoved(true);
    reported.setRating(0.9F);
    reported.setTitle(u"Changed"_s);
    publish(*library, {reported}, Track::Stat::Loved | Track::Stat::Rating);
    const Track published = context().library.trackForId(track.id());
    EXPECT_TRUE(published.isLoved());
    EXPECT_FLOAT_EQ(published.rating(), 0.4F);
    EXPECT_EQ(published.title(), u"Item"_s);
    EXPECT_EQ(published.libraryId(), library->id);
    EXPECT_EQ(published.readOnlyStats(), Track::Stats{Track::Stat::Loved});
    ASSERT_EQ(statsChanged.size(), 1);
    EXPECT_EQ(statsChanged.front().at(1).value<Track::Stats>(), Track::Stats{Track::Stat::Loved});
    EXPECT_FALSE(context().library.trackForId(foreignTrack.id()).isLoved());
    ASSERT_TRUE(waitForFinished());
    EXPECT_EQ(finished.front().at(0).toUuid(), operations.front());
    EXPECT_EQ(finished.front().at(1).toInt(), library->id);
    EXPECT_TRUE(finished.front().at(2).toBool());
    EXPECT_TRUE(requested.empty());

    Track unloved{track};
    unloved.setLoved(false);
    publish(*library, {unloved}, Track::Stat::Loved);
    publish(*library, {reported}, Track::Stat::Loved);
    publish(*library, {unloved}, Track::Stat::Loved);
    EXPECT_EQ(statsChanged.size(), 4);
    EXPECT_FALSE(context().library.trackForId(track.id()).isLoved());
    ASSERT_TRUE(waitForFinished());
    for(qsizetype i{1}; i < finished.size(); ++i) {
        EXPECT_EQ(finished.at(i).at(0).toUuid(), operations.at(i));
        EXPECT_TRUE(finished.at(i).at(2).toBool());
    }
    EXPECT_FALSE(context().library.trackForId(track.id()).isLoved());

    publish(*library, {reported}, Track::Stat::Loved);
    source.setLoved(false);
    context().library.applyExternalLibrarySnapshot(*library, {source}, QUuid::createUuid());
    ASSERT_TRUE(waitForSignal(applied).at(2).toBool());
    EXPECT_FALSE(context().library.trackForId(track.id()).isLoved());
    publish(*library, {reported}, Track::Stat::Loved);
    EXPECT_TRUE(context().library.trackForId(track.id()).isLoved());
    ASSERT_TRUE(waitForFinished());

    Track foreignLoved{foreignTrack};
    foreignLoved.setLoved(true);
    LibraryInfo unregistered{*library};
    unregistered.id = library->id + 100;
    LibraryInfo moved{*library};
    moved.path = u"test-source://other"_s;
    const qsizetype acceptedCount = finished.size();
    const qsizetype changedCount  = statsChanged.size();
    publish(unregistered, {unloved}, Track::Stat::Loved);
    publish(moved, {unloved}, Track::Stat::Loved);
    publish(*foreign, {unloved}, Track::Stat::Loved);
    publish(*library, {unloved, foreignLoved}, Track::Stat::Loved);
    publish(*library, {unloved}, Track::Stat::Rating);
    context().library.applyExternalTrackStats(*library, {unloved}, Track::Stat::Loved, QUuid{});
    EXPECT_EQ(statsChanged.size(), changedCount);
    EXPECT_TRUE(context().library.trackForId(track.id()).isLoved());
    ASSERT_TRUE(waitForCondition([&]() { return finished.size() == acceptedCount + 6; }));
    for(qsizetype i{acceptedCount}; i < acceptedCount + 5; ++i) {
        EXPECT_EQ(finished.at(i).at(0).toUuid(), operations.at(i));
        EXPECT_FALSE(finished.at(i).at(2).toBool());
    }
    EXPECT_TRUE(finished.at(acceptedCount + 5).at(0).toUuid().isNull());
    EXPECT_FALSE(finished.at(acceptedCount + 5).at(2).toBool());
    operations.emplace_back();

    publish(*foreign, {foreignLoved}, Track::Stat::Loved);
    EXPECT_TRUE(context().library.trackForId(foreignTrack.id()).isLoved());
    ASSERT_TRUE(waitForFinished());
    EXPECT_TRUE(finished.back().at(2).toBool());
    EXPECT_TRUE(context().library.trackForId(track.id()).isLoved());
    EXPECT_TRUE(requested.empty());

    UnifiedMusicLibrary reloaded{&context().libraryManager, context().dbPool,   context().playlistLoader,
                                 context().audioLoader,     context().remoteIo, &context().settings};
    QSignalSpy loaded{&reloaded, &MusicLibrary::tracksLoaded};
    reloaded.loadAllTracks();
    waitForSignal(loaded);
    EXPECT_TRUE(reloaded.trackForId(track.id()).isLoved());
    EXPECT_FLOAT_EQ(reloaded.trackForId(track.id()).rating(), 0.4F);
    EXPECT_EQ(reloaded.trackForId(track.id()).libraryId(), library->id);
    EXPECT_TRUE(reloaded.trackForId(foreignTrack.id()).isLoved());
}

TEST_F(UnifiedMusicLibraryTest, ExternalStatsPublicationIsVisibleWhilePersistenceIsHeld)
{
    const auto library = context().library.addExternalLibrary(u"test-source://optimistic"_s, u"External"_s);
    ASSERT_TRUE(library);
    context().library.setExternalLibraryReadOnlyStats(*library, Track::Stat::Loved);
    Track source = externalTrack(u"test-source://optimistic/item"_s, u"Item"_s);
    QSignalSpy applied{&context().library, &MusicLibrary::externalLibrarySnapshotFinished};
    context().library.applyExternalLibrarySnapshot(*library, {source}, QUuid::createUuid());
    ASSERT_TRUE(waitForSignal(applied).at(2).toBool());
    const Track track = tracksByPath(context().library.libraryTracks()).at(u"test-source://optimistic/item"_s);

    QObject handler;
    context().library.setExternalLibraryStatsHandler(*library, Track::Stat::Loved, &handler);
    std::vector<bool> requestedLoved;
    QObject::connect(&context().library, &MusicLibrary::externalTrackStatsChangeRequested, &handler,
                     [&](const LibraryInfo& requestLibrary, const TrackList& tracks, Track::Stats stats) {
                         requestedLoved.push_back(tracks.front().isLoved());
                         context().library.applyExternalTrackStats(requestLibrary, tracks, stats, QUuid::createUuid());
                     });
    QSignalSpy updated{&context().library, &MusicLibrary::tracksUpdated};
    QSignalSpy finished{&context().library, &MusicLibrary::externalTrackStatsFinished};

    auto toggleLoved = [&]() {
        Track edited = context().library.trackForId(track.id());
        edited.setLoved(!edited.isLoved());
        context().library.editTrackStats({edited}, Track::Stat::Loved);
        return context().library.trackForId(track.id()).isLoved();
    };

    {
        LibrarySortBarrier storage;
        source.setTitle(u"Renamed"_s);
        context().library.applyExternalLibrarySnapshot(*library, {source}, QUuid::createUuid());

        EXPECT_TRUE(toggleLoved());
        ASSERT_EQ(updated.size(), 1);
        EXPECT_TRUE(updated.front().at(0).value<TrackList>().front().isLoved());
        EXPECT_FALSE(toggleLoved());
        EXPECT_TRUE(toggleLoved());
        EXPECT_EQ(requestedLoved, (std::vector{true, false, true}));
        QCoreApplication::processEvents();
        EXPECT_TRUE(finished.empty());
    }

    ASSERT_TRUE(waitForSignal(applied).at(2).toBool());
    ASSERT_TRUE(waitForCondition([&]() { return finished.size() == 3; }));
    for(const QVariantList& result : finished) {
        EXPECT_TRUE(result.at(2).toBool());
    }
    const Track settled = context().library.trackForId(track.id());
    EXPECT_EQ(settled.title(), u"Renamed"_s);
    EXPECT_TRUE(settled.isLoved());

    TrackDatabase database;
    database.initialise(DbConnectionProvider{context().dbPool});
    Track stored{track};
    ASSERT_TRUE(database.reloadTrack(stored));
    EXPECT_TRUE(stored.isLoved());
}

TEST_F(UnifiedMusicLibraryTest, ExternalStatsPublicationStaysVisibleWhenStorageFails)
{
    const auto library = context().library.addExternalLibrary(u"test-source://failing"_s, u"External"_s);
    ASSERT_TRUE(library);
    context().library.setExternalLibraryReadOnlyStats(*library, Track::Stat::Loved);
    const auto localLibrary = context().library.addExternalLibrary(u"test-source://local"_s, u"Local"_s);
    ASSERT_TRUE(localLibrary);
    QSignalSpy applied{&context().library, &MusicLibrary::externalLibrarySnapshotFinished};
    context().library.applyExternalLibrarySnapshot(
        *library, {externalTrack(u"test-source://failing/item"_s, u"Item"_s)}, QUuid::createUuid());
    ASSERT_TRUE(waitForSignal(applied).at(2).toBool());
    context().library.applyExternalLibrarySnapshot(
        *localLibrary, {externalTrack(u"test-source://local/item"_s, u"Local"_s)}, QUuid::createUuid());
    ASSERT_TRUE(waitForSignal(applied).at(2).toBool());
    const Track track      = tracksByPath(context().library.libraryTracks()).at(u"test-source://failing/item"_s);
    const Track localTrack = tracksByPath(context().library.libraryTracks()).at(u"test-source://local/item"_s);

    QSignalSpy requested{&context().library, &MusicLibrary::externalTrackStatsChangeRequested};
    QSignalSpy statsChanged{&context().library, &MusicLibrary::tracksStatsChanged};
    QSignalSpy finished{&context().library, &MusicLibrary::externalTrackStatsFinished};

    const DbConnectionProvider dbProvider{context().dbPool};
    DbQuery failInsert{dbProvider.db(), u"CREATE TRIGGER FailTrackStats BEFORE INSERT ON TrackStats "
                                        u"BEGIN SELECT RAISE(ABORT, 'synthetic failure'); END;"_s};
    ASSERT_TRUE(failInsert.exec()) << failInsert.lastError().text().toStdString();

    Track loved{track};
    loved.setLoved(true);
    const QUuid operation = QUuid::createUuid();
    context().library.applyExternalTrackStats(*library, {loved}, Track::Stat::Loved, operation);
    EXPECT_TRUE(context().library.trackForId(track.id()).isLoved());

    const QVariantList failure = waitForSignal(finished);
    ASSERT_EQ(failure.size(), 3);
    EXPECT_EQ(failure.at(0).toUuid(), operation);
    EXPECT_EQ(failure.at(1).toInt(), library->id);
    EXPECT_FALSE(failure.at(2).toBool());
    EXPECT_TRUE(context().library.trackForId(track.id()).isLoved());

    TrackDatabase database;
    database.initialise(DbConnectionProvider{context().dbPool});
    Track stored{track};
    ASSERT_TRUE(database.reloadTrack(stored));
    EXPECT_FALSE(stored.isLoved());

    DbQuery dropInsert{dbProvider.db(), u"DROP TRIGGER FailTrackStats;"_s};
    ASSERT_TRUE(dropInsert.exec()) << dropInsert.lastError().text().toStdString();

    Track rated{track};
    rated.setRating(0.4F);
    Track localLoved{localTrack};
    localLoved.setLoved(true);
    context().library.updateTrackStats(rated, Track::Stat::Rating);
    context().library.updateTrackStats(localLoved, Track::Stat::Loved);
    ASSERT_TRUE(waitForCondition([&]() {
        return context().library.trackForId(track.id()).rating() == 0.4F
            && context().library.trackForId(localTrack.id()).isLoved();
    }));
    EXPECT_TRUE(context().library.trackForId(track.id()).isLoved());

    QCoreApplication::processEvents();
    EXPECT_TRUE(finished.empty());
    EXPECT_TRUE(requested.empty());
    for(const QVariantList& change : statsChanged) {
        for(const Track& changedTrack : change.at(0).value<TrackList>()) {
            if(changedTrack.id() == track.id()) {
                EXPECT_TRUE(changedTrack.isLoved());
            }
        }
    }
    ASSERT_TRUE(database.reloadTrack(stored));
    EXPECT_FALSE(stored.isLoved());
    EXPECT_FLOAT_EQ(stored.rating(), 0.4F);
}

TEST_F(UnifiedMusicLibraryTest, ExternalStatsRollbackStaysVisibleWhenItsStorageFails)
{
    const auto library = context().library.addExternalLibrary(u"test-source://rollback"_s, u"External"_s);
    ASSERT_TRUE(library);
    context().library.setExternalLibraryReadOnlyStats(*library, Track::Stat::Loved);
    QSignalSpy applied{&context().library, &MusicLibrary::externalLibrarySnapshotFinished};
    context().library.applyExternalLibrarySnapshot(
        *library, {externalTrack(u"test-source://rollback/item"_s, u"Item"_s)}, QUuid::createUuid());
    ASSERT_TRUE(waitForSignal(applied).at(2).toBool());
    const Track track = tracksByPath(context().library.libraryTracks()).at(u"test-source://rollback/item"_s);

    QSignalSpy statsChanged{&context().library, &MusicLibrary::tracksStatsChanged};
    QSignalSpy finished{&context().library, &MusicLibrary::externalTrackStatsFinished};

    Track optimistic{track};
    optimistic.setLoved(true);
    const QUuid optimisticOperation = QUuid::createUuid();
    context().library.applyExternalTrackStats(*library, {optimistic}, Track::Stat::Loved, optimisticOperation);
    const QVariantList optimisticResult = waitForSignal(finished);
    EXPECT_EQ(optimisticResult.at(0).toUuid(), optimisticOperation);
    EXPECT_TRUE(optimisticResult.at(2).toBool());

    const DbConnectionProvider dbProvider{context().dbPool};
    DbQuery failInsert{dbProvider.db(), u"CREATE TRIGGER FailTrackStats BEFORE INSERT ON TrackStats "
                                        u"BEGIN SELECT RAISE(ABORT, 'synthetic failure'); END;"_s};
    ASSERT_TRUE(failInsert.exec()) << failInsert.lastError().text().toStdString();

    Track confirmed{track};
    confirmed.setLoved(false);
    const QUuid rollbackOperation = QUuid::createUuid();
    context().library.applyExternalTrackStats(*library, {confirmed}, Track::Stat::Loved, rollbackOperation);
    EXPECT_FALSE(context().library.trackForId(track.id()).isLoved());

    const QVariantList rollbackResult = waitForSignal(finished);
    EXPECT_EQ(rollbackResult.at(0).toUuid(), rollbackOperation);
    EXPECT_FALSE(rollbackResult.at(2).toBool());
    EXPECT_FALSE(context().library.trackForId(track.id()).isLoved());

    QCoreApplication::processEvents();
    EXPECT_TRUE(finished.empty());
    ASSERT_EQ(statsChanged.size(), 2);
    EXPECT_TRUE(statsChanged.at(0).at(0).value<TrackList>().front().isLoved());
    EXPECT_FALSE(statsChanged.at(1).at(0).value<TrackList>().front().isLoved());
    EXPECT_FALSE(context().library.trackForId(track.id()).isLoved());

    TrackDatabase database;
    database.initialise(DbConnectionProvider{context().dbPool});
    Track stored{track};
    ASSERT_TRUE(database.reloadTrack(stored));
    EXPECT_TRUE(stored.isLoved());

    DbQuery dropInsert{dbProvider.db(), u"DROP TRIGGER FailTrackStats;"_s};
    ASSERT_TRUE(dropInsert.exec()) << dropInsert.lastError().text().toStdString();
}

TEST_F(UnifiedMusicLibraryTest, ExternalStatsDisjointMasksSettleIndependently)
{
    const auto library = context().library.addExternalLibrary(u"test-source://masks"_s, u"External"_s);
    ASSERT_TRUE(library);
    context().library.setExternalLibraryReadOnlyStats(*library, Track::Stat::Loved | Track::Stat::Rating);
    QSignalSpy applied{&context().library, &MusicLibrary::externalLibrarySnapshotFinished};
    context().library.applyExternalLibrarySnapshot(*library, {externalTrack(u"test-source://masks/item"_s, u"Item"_s)},
                                                   QUuid::createUuid());
    ASSERT_TRUE(waitForSignal(applied).at(2).toBool());
    const Track track = tracksByPath(context().library.libraryTracks()).at(u"test-source://masks/item"_s);

    const DbConnectionProvider dbProvider{context().dbPool};
    DbQuery failInsert{dbProvider.db(), u"CREATE TRIGGER FailTrackStats BEFORE INSERT ON TrackStats "
                                        u"WHEN NEW.Loved = 1 "
                                        u"BEGIN SELECT RAISE(ABORT, 'synthetic failure'); END;"_s};
    ASSERT_TRUE(failInsert.exec()) << failInsert.lastError().text().toStdString();

    QSignalSpy statsChanged{&context().library, &MusicLibrary::tracksStatsChanged};
    QSignalSpy finished{&context().library, &MusicLibrary::externalTrackStatsFinished};

    Track loved{track};
    loved.setLoved(true);
    Track rated{track};
    rated.setRating(0.8F);
    const QUuid lovedOperation  = QUuid::createUuid();
    const QUuid ratingOperation = QUuid::createUuid();
    context().library.applyExternalTrackStats(*library, {loved}, Track::Stat::Loved, lovedOperation);
    context().library.applyExternalTrackStats(*library, {rated}, Track::Stat::Rating, ratingOperation);
    const Track published = context().library.trackForId(track.id());
    EXPECT_TRUE(published.isLoved());
    EXPECT_FLOAT_EQ(published.rating(), 0.8F);

    ASSERT_TRUE(waitForCondition([&]() { return finished.size() == 2; }));
    QCoreApplication::processEvents();
    ASSERT_EQ(finished.size(), 2);
    EXPECT_EQ(finished.at(0).at(0).toUuid(), lovedOperation);
    EXPECT_FALSE(finished.at(0).at(2).toBool());
    EXPECT_EQ(finished.at(1).at(0).toUuid(), ratingOperation);
    EXPECT_TRUE(finished.at(1).at(2).toBool());
    ASSERT_EQ(statsChanged.size(), 2);
    EXPECT_EQ(statsChanged.at(0).at(1).value<Track::Stats>(), Track::Stats{Track::Stat::Loved});
    EXPECT_EQ(statsChanged.at(1).at(1).value<Track::Stats>(), Track::Stats{Track::Stat::Rating});

    const Track settled = context().library.trackForId(track.id());
    EXPECT_TRUE(settled.isLoved());
    EXPECT_FLOAT_EQ(settled.rating(), 0.8F);

    TrackDatabase database;
    database.initialise(DbConnectionProvider{context().dbPool});
    Track stored{track};
    ASSERT_TRUE(database.reloadTrack(stored));
    EXPECT_FALSE(stored.isLoved());
    EXPECT_FLOAT_EQ(stored.rating(), 0.8F);

    UnifiedMusicLibrary reloaded{&context().libraryManager, context().dbPool,   context().playlistLoader,
                                 context().audioLoader,     context().remoteIo, &context().settings};
    QSignalSpy loaded{&reloaded, &MusicLibrary::tracksLoaded};
    reloaded.loadAllTracks();
    waitForSignal(loaded);
    const Track reloadedTrack = reloaded.trackForId(track.id());
    EXPECT_FALSE(reloadedTrack.isLoved());
    EXPECT_FLOAT_EQ(reloadedTrack.rating(), 0.8F);

    DbQuery dropInsert{dbProvider.db(), u"DROP TRIGGER FailTrackStats;"_s};
    ASSERT_TRUE(dropInsert.exec()) << dropInsert.lastError().text().toStdString();
}

TEST_F(UnifiedMusicLibraryTest, ExternalStatsPublicationReplacesPlayTimesExactly)
{
    const auto library = context().library.addExternalLibrary(u"test-source://played"_s, u"External"_s);
    ASSERT_TRUE(library);
    context().library.setExternalLibraryReadOnlyStats(*library, Track::Stat::Playcount);
    Track source = externalTrack(u"test-source://played/item"_s, u"Item"_s);
    source.setPlayCount(5);
    source.setFirstPlayed(1000);
    source.setLastPlayed(5000);
    QSignalSpy applied{&context().library, &MusicLibrary::externalLibrarySnapshotFinished};
    context().library.applyExternalLibrarySnapshot(*library, {source}, QUuid::createUuid());
    ASSERT_TRUE(waitForSignal(applied).at(2).toBool());
    const Track track = tracksByPath(context().library.libraryTracks()).at(u"test-source://played/item"_s);
    ASSERT_EQ(track.playCount(), 5);
    ASSERT_EQ(track.firstPlayed(), 1000U);
    ASSERT_EQ(track.lastPlayed(), 5000U);

    Track rated{track};
    rated.setRating(0.4F);
    context().library.updateTrackStats(rated, Track::Stat::Rating);
    ASSERT_TRUE(waitForCondition([&]() { return context().library.trackForId(track.id()).rating() == 0.4F; }));

    QSignalSpy finished{&context().library, &MusicLibrary::externalTrackStatsFinished};
    TrackDatabase database;
    database.initialise(DbConnectionProvider{context().dbPool});

    Track reduced{u"test-source://played/values"_s};
    reduced.setPlayCount(2);
    reduced.setFirstPlayed(2000);
    reduced.setLastPlayed(3000);
    Track reported{track};
    reported.replaceStats(reduced, Track::Stat::Playcount);
    context().library.applyExternalTrackStats(*library, {reported}, Track::Stat::Playcount, QUuid::createUuid());
    const Track published = context().library.trackForId(track.id());
    EXPECT_EQ(published.playCount(), 2);
    EXPECT_EQ(published.firstPlayed(), 2000U);
    EXPECT_EQ(published.lastPlayed(), 3000U);
    EXPECT_FLOAT_EQ(published.rating(), 0.4F);
    ASSERT_TRUE(waitForSignal(finished).at(2).toBool());

    Track stored{track};
    ASSERT_TRUE(database.reloadTrack(stored));
    EXPECT_EQ(stored.playCount(), 2);
    EXPECT_EQ(stored.firstPlayed(), 2000U);
    EXPECT_EQ(stored.lastPlayed(), 3000U);
    EXPECT_FLOAT_EQ(stored.rating(), 0.4F);

    reported.replaceStats(Track{u"test-source://played/reset"_s}, Track::Stat::Playcount);
    context().library.applyExternalTrackStats(*library, {reported}, Track::Stat::Playcount, QUuid::createUuid());
    const Track reset = context().library.trackForId(track.id());
    EXPECT_EQ(reset.playCount(), 0);
    EXPECT_EQ(reset.firstPlayed(), 0U);
    EXPECT_EQ(reset.lastPlayed(), 0U);
    ASSERT_TRUE(waitForSignal(finished).at(2).toBool());

    ASSERT_TRUE(database.reloadTrack(stored));
    EXPECT_EQ(stored.playCount(), 0);
    EXPECT_EQ(stored.firstPlayed(), 0U);
    EXPECT_EQ(stored.lastPlayed(), 0U);

    UnifiedMusicLibrary reloaded{&context().libraryManager, context().dbPool,   context().playlistLoader,
                                 context().audioLoader,     context().remoteIo, &context().settings};
    QSignalSpy loaded{&reloaded, &MusicLibrary::tracksLoaded};
    reloaded.loadAllTracks();
    waitForSignal(loaded);
    const Track reloadedTrack = reloaded.trackForId(track.id());
    EXPECT_EQ(reloadedTrack.playCount(), 0);
    EXPECT_EQ(reloadedTrack.firstPlayed(), 0U);
    EXPECT_EQ(reloadedTrack.lastPlayed(), 0U);
    EXPECT_FLOAT_EQ(reloadedTrack.rating(), 0.4F);
}

#ifdef FOOYIN_TEST_TAGEDITOR
TEST_F(UnifiedMusicLibraryTest, TagEditorLovedEditsUseTheExternalStatsHandler)
{
    const auto library = context().library.addExternalLibrary(u"test-source://tagged"_s, u"External"_s);
    ASSERT_TRUE(library);
    context().library.setExternalLibraryReadOnlyStats(*library, Track::Stat::Loved);
    QSignalSpy applied{&context().library, &MusicLibrary::externalLibrarySnapshotFinished};
    context().library.applyExternalLibrarySnapshot(*library, {externalTrack(u"test-source://tagged/item"_s, u"Item"_s)},
                                                   QUuid::createUuid());
    ASSERT_TRUE(waitForSignal(applied).at(2).toBool());
    const Track track = tracksByPath(context().library.libraryTracks()).at(u"test-source://tagged/item"_s);

    std::vector<std::pair<TrackList, Track::Stats>> requests;
    QObject::connect(&context().library, &MusicLibrary::externalTrackStatsChangeRequested, &context().library,
                     [&requests](const LibraryInfo& /*library*/, const TrackList& tracks, Track::Stats stats) {
                         requests.emplace_back(tracks, stats);
                     });

    Track edited{track};
    edited.setLoved(true);
    edited.setRating(0.6F);
    TagEditor::saveTrackStats(&context().library, {edited}, Track::Stat::Loved | Track::Stat::Rating);
    ASSERT_TRUE(waitForCondition([&]() { return context().library.trackForId(track.id()).rating() == 0.6F; }));
    EXPECT_TRUE(requests.empty());
    EXPECT_FALSE(context().library.trackForId(track.id()).isLoved());

    QObject handler;
    context().library.setExternalLibraryStatsHandler(*library, Track::Stat::Loved, &handler);
    edited.setRating(0.8F);
    TagEditor::saveTrackStats(&context().library, {edited}, Track::Stat::Loved | Track::Stat::Rating);
    ASSERT_EQ(requests.size(), 1U);
    EXPECT_EQ(requests.front().second, Track::Stats{Track::Stat::Loved});
    ASSERT_EQ(requests.front().first.size(), 1U);
    EXPECT_EQ(requests.front().first.front().id(), track.id());
    EXPECT_TRUE(requests.front().first.front().isLoved());
    ASSERT_TRUE(waitForCondition([&]() { return context().library.trackForId(track.id()).rating() == 0.8F; }));
    EXPECT_FALSE(context().library.trackForId(track.id()).isLoved());

    TagEditor::saveTrackStats(&context().library, {edited}, Track::Stat::Rating);
    EXPECT_EQ(requests.size(), 1U);

    context().library.setExternalLibraryActive(*library, false);
    TagEditor::saveTrackStats(&context().library, {edited}, Track::Stat::Loved);
    EXPECT_EQ(requests.size(), 1U);
    context().library.setExternalLibraryActive(*library, true);
    TagEditor::saveTrackStats(&context().library, {edited}, Track::Stat::Loved);
    EXPECT_EQ(requests.size(), 2U);
}

TEST_F(UnifiedMusicLibraryTest, TagEditorPropertiesTabLovedFollowsTheSourceCapability)
{
    auto& settings = context().settings;
    settings.createTempSetting<Settings::Gui::TrackEditorAppearance>(QVariant::fromValue(TrackEditorAppearance{}));
    settings.createSetting(QString::fromLatin1(TagEditor::SettingsKeys::MultiValueSeparators),
                           QString::fromLatin1(TagEditor::SettingsKeys::DefaultMultiValueSeparators));
    settings.createSetting(QString::fromLatin1(TagEditor::SettingsKeys::AutoFillMultiValueSeparators),
                           QString::fromLatin1(TagEditor::SettingsKeys::DefaultMultiValueSeparators));
    settings.createSetting(QString::fromLatin1(TagEditor::SettingsKeys::EditValuesOnSingleClick), true);
    settings.fileSet(u"TagEditor/DontAskAgain"_s, true);

    const auto library = context().library.addExternalLibrary(u"test-source://properties"_s, u"External"_s);
    ASSERT_TRUE(library);
    context().library.setExternalLibraryReadOnlyStats(*library, Track::Stat::Loved);
    QSignalSpy applied{&context().library, &MusicLibrary::externalLibrarySnapshotFinished};
    context().library.applyExternalLibrarySnapshot(
        *library, {externalTrack(u"test-source://properties/item"_s, u"Item"_s)}, QUuid::createUuid());
    ASSERT_TRUE(waitForSignal(applied).at(2).toBool());
    const Track track = tracksByPath(context().library.libraryTracks()).at(u"test-source://properties/item"_s);

    std::vector<std::pair<TrackList, Track::Stats>> requests;
    QObject::connect(&context().library, &MusicLibrary::externalTrackStatsChangeRequested, &context().library,
                     [&requests](const LibraryInfo& /*library*/, const TrackList& tracks, Track::Stats stats) {
                         requests.emplace_back(tracks, stats);
                     });

    QMainWindow mainWindow;
    ActionManager actionManager{&settings};
    actionManager.setMainWindow(&mainWindow);
    TagEditor::TagEditorFieldRegistry registry{&settings};
    TagEditor::TagEditorPropertiesTab tab{&actionManager, &registry, &context().library, &settings};
    QObject::connect(&tab, &TagEditor::TagEditorPropertiesTab::trackStatsChanged, &context().library,
                     [this](const TrackList& tracks, Track::Stats stats) {
                         TagEditor::saveTrackStats(&context().library, tracks, stats);
                     });

    auto* model = tab.findChild<TagEditor::TagEditorModel*>();
    ASSERT_NE(model, nullptr);
    const auto lovedIndex = [model]() {
        for(int row{0}; row < model->rowCount({}); ++row) {
            const QModelIndex index = model->index(row, 1, {});
            if(static_cast<TagEditor::TagEditorItem*>(index.internalPointer())
                   ->field()
                   .scriptField.compare(QLatin1StringView{Constants::MetaData::LoveEditor}, Qt::CaseInsensitive)
               == 0) {
                return index;
            }
        }
        return QModelIndex{};
    };

    tab.setTracks({track});
    ASSERT_TRUE(waitForCondition([&]() { return lovedIndex().isValid(); }));

    EXPECT_FALSE(model->flags(lovedIndex()).testFlag(Qt::ItemIsEnabled));
    EXPECT_FALSE(model->data(lovedIndex(), Qt::ToolTipRole).toString().isEmpty());
    EXPECT_FALSE(model->setData(lovedIndex(), u"1"_s, Qt::EditRole));
    EXPECT_FALSE(tab.hasPendingScopeChanges());

    auto handler = std::make_unique<QObject>();
    context().library.setExternalLibraryStatsHandler(*library, Track::Stat::Loved, handler.get());
    EXPECT_TRUE(model->flags(lovedIndex()).testFlag(Qt::ItemIsEnabled));
    EXPECT_TRUE(model->data(lovedIndex(), Qt::ToolTipRole).toString().isEmpty());
    ASSERT_TRUE(model->setData(lovedIndex(), u"1"_s, Qt::EditRole));
    ASSERT_TRUE(tab.hasPendingScopeChanges());

    handler.reset();
    EXPECT_FALSE(model->flags(lovedIndex()).testFlag(Qt::ItemIsEnabled));
    EXPECT_TRUE(tab.hasPendingScopeChanges());

    int warnings{0};
    QTimer closeWarning;
    QObject::connect(&closeWarning, &QTimer::timeout, &closeWarning, [&warnings]() {
        if(auto* dialog = QApplication::activeModalWidget()) {
            ++warnings;
            dialog->close();
        }
    });
    closeWarning.start(10);
    tab.apply();
    closeWarning.stop();
    EXPECT_EQ(warnings, 1);
    EXPECT_TRUE(requests.empty());
    EXPECT_FALSE(context().library.trackForId(track.id()).isLoved());

    QObject restoredHandler;
    context().library.setExternalLibraryStatsHandler(*library, Track::Stat::Loved, &restoredHandler);
    context().library.setExternalLibraryActive(*library, false);
    EXPECT_FALSE(model->flags(lovedIndex()).testFlag(Qt::ItemIsEnabled));
    context().library.setExternalLibraryActive(*library, true);
    EXPECT_TRUE(model->flags(lovedIndex()).testFlag(Qt::ItemIsEnabled));

    tab.apply();
    ASSERT_EQ(requests.size(), 1U);
    EXPECT_EQ(requests.front().second, Track::Stats{Track::Stat::Loved});
    ASSERT_EQ(requests.front().first.size(), 1U);
    EXPECT_EQ(requests.front().first.front().id(), track.id());
    EXPECT_TRUE(requests.front().first.front().isLoved());
}
#endif
} // namespace Fooyin::Testing
