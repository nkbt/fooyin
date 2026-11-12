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

#include "fycore_export.h"

#include <core/library/libraryinfo.h>
#include <core/track.h>
#include <utils/database/dbmodule.h>

#include <QCoreApplication>

#include <expected>
#include <memory>
#include <optional>
#include <set>

namespace Fooyin {
class TrackMetadataStore;

class FYCORE_EXPORT TrackDatabase : public DbModule
{
    Q_DECLARE_TR_FUNCTIONS(Fooyin::TrackDatabase)

public:
    void setMetadataStore(std::shared_ptr<TrackMetadataStore> store);

    bool storeTracks(TrackList& tracks);
    /*!
     * Inserts @p addedTracks and updates @p updatedTracks in one transaction while @p library is still registered.
     * Fails without changes if a track belongs to another library. Statistics in @p sourceStats replace the stored
     * values of tracks in @p library; other statistics keep their stored values. Both lists are reloaded as stored.
     */
    bool storeExternalSnapshot(const LibraryInfo& library, TrackList& addedTracks, TrackList& updatedTracks,
                               Track::Stats sourceStats);
    bool updateTracks(TrackList& tracks);
    std::expected<void, QString> writeScanBatch(TrackList& tracksToStore, TrackList& tracksToUpdate);

    bool reloadTrack(Track& track) const;
    bool reloadTracks(TrackList& tracks) const;
    [[nodiscard]] TrackList getAllTracks() const;
    [[nodiscard]] TrackList tracksByHash(const QString& hash) const;
    int idForTrack(Track& track) const;

    bool updateTrack(const Track& track);
    bool updateTrackStats(const Track& track);
    bool updateTrackStats(Track& track, Track::Stats updatedStats);
    bool updateTrackStats(const TrackList& tracks);

    bool deleteTrack(int id);
    bool deleteTracks(const TrackList& tracks);
    /*!
     * Deletes the tracks of @p library that no playlist references and detaches the others. For an external library, it also clears the source-owned statistics of every stored track from that source, including tracks detached earlier. Returns the deleted track ids, or std::nullopt if a query fails. Callers provide the transaction.
     */
    std::optional<std::set<int>> deleteLibraryTracks(const LibraryInfo& library);

    void cleanupTracks();

    static void dropViews(const QSqlDatabase& db);
    static void insertViews(const QSqlDatabase& db);

private:
    [[nodiscard]] int trackCount() const;
    bool storeTrack(Track& track, Track::Stats sourceStats);
    bool updateTrackData(const Track& track);
    bool insertTrack(Track& track, bool ignoreDuplicates = false) const;

    struct StoredTrackStats
    {
        uint64_t added{0};
        uint64_t firstPlayed{0};
        uint64_t lastPlayed{0};
        int playCount{0};
        float rating{-1.0F};
        bool loved{false};
    };
    std::optional<StoredTrackStats> existingTrackStats(const QString& hash) const;
    bool insertOrUpdateStats(const Track& track, StoredTrackStats* mergedStats, Track::Stats updatedStats,
                             Track::Stats sourceStats) const;

    void removeUnmanagedTracks() const;
    void updateLastSeenStats() const;
    void deleteExpiredStats() const;

    std::shared_ptr<TrackMetadataStore> m_metadataStore;
};
} // namespace Fooyin
