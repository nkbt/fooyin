/*
 * Fooyin
 * Copyright © 2023, Luke Taylor <luket@pm.me>
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

#pragma once

#include "libraryscanner.h"

#include <core/library/musiclibrary.h>

namespace Fooyin {
struct LibraryInfo;
class LibraryManager;
class RemoteIoService;
class SettingsManager;
class UnifiedMusicLibraryPrivate;

class FYCORE_EXPORT UnifiedMusicLibrary : public MusicLibrary
{
    Q_OBJECT

public:
    UnifiedMusicLibrary(LibraryManager* libraryManager, DbConnectionPoolPtr dbPool,
                        std::shared_ptr<PlaylistLoader> playlistLoader, std::shared_ptr<AudioLoader> audioLoader,
                        std::shared_ptr<RemoteIoService> remoteIo, SettingsManager* settings,
                        QObject* parent = nullptr);
    ~UnifiedMusicLibrary() override;

    [[nodiscard]] bool hasLibrary() const override;
    [[nodiscard]] std::optional<LibraryInfo> libraryInfo(int id) const override;
    [[nodiscard]] std::optional<LibraryInfo> libraryForPath(const QString& path) const override;

    void loadAllTracks() override;

    [[nodiscard]] bool isEmpty() const override;

    void refreshAll() override;
    void rescanAll() override;

    ScanRequest refresh(const LibraryInfo& library) override;
    ScanRequest rescan(const LibraryInfo& library) override;
    void cancelScan(int id) override;

    ScanRequest scanTracks(const TrackList& tracks) override;
    ScanRequest scanModifiedTracks(const TrackList& tracks) override;
    ScanRequest scanFiles(const QList<QUrl>& files) override;
    ScanRequest loadPlaylist(const QList<QUrl>& files) override;

    [[nodiscard]] TrackList tracks() const override;
    [[nodiscard]] TrackList libraryTracks() const override;
    [[nodiscard]] Track trackForId(int id) const override;
    [[nodiscard]] TrackList tracksForIds(const TrackIds& ids) const override;
    [[nodiscard]] std::shared_ptr<TrackMetadataStore> metadataStore() const override;

    void updateTrack(const Track& track) override;
    void updateTracks(const TrackList& tracks) override;

    void updateTrackMetadata(const TrackList& tracks) override;
    WriteRequest writeTrackMetadata(const TrackList& tracks) override;
    WriteRequest writeTrackCovers(const TrackCoverData& tracks) override;
    [[nodiscard]] PendingTrackCoverProvider* pendingTrackCoverProvider() const override;

    void updateTrackStats(const TrackList& tracks, Track::Stats stats) override;
    void updateTrackStats(const Track& track, Track::Stats stats) override;
    void editTrackStats(const TrackList& tracks, Track::Stats stats) override;

    void trackWasPlayed(const Track& track);
    void setActivePlaybackTrack(const Track& track);
    void flushPendingWrites();
    void cleanupTracks();
    WriteRequest removeUnavailbleTracks() override;
    WriteRequest deleteTracks(const TrackList& tracks) override;
    std::optional<LibraryInfo> addExternalLibrary(const QString& path, const QString& name) override;
    void applyExternalLibrarySnapshot(const LibraryInfo& library, const TrackList& tracks,
                                      const QUuid& operation) override;
    bool isTrackSourceActive(const Track& track) const override;
    void setExternalLibraryActive(const LibraryInfo& library, bool active) override;
    void setExternalLibraryReadOnlyStats(const LibraryInfo& library, Track::Stats stats) override;
    void setExternalLibraryStatsHandler(const LibraryInfo& library, Track::Stats stats, QObject* handler) override;
    [[nodiscard]] bool canEditTrackStats(const Track& track, Track::Stats stats) const override;
    void applyExternalTrackStats(const LibraryInfo& library, const TrackList& tracks, Track::Stats stats,
                                 const QUuid& operation) override;

private:
    std::unique_ptr<UnifiedMusicLibraryPrivate> p;
};
} // namespace Fooyin
