/*
 * Fooyin
 * Copyright © 2026, Nikita Butenko
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

#include "tageditorstats.h"

#include <core/library/musiclibrary.h>

#include <algorithm>
#include <iterator>

namespace Fooyin::TagEditor {
void saveTrackStats(MusicLibrary* library, const TrackList& tracks, const Track::Stats stats)
{
    if(stats.testFlag(Track::Stat::Loved)) {
        library->editTrackStats(tracks, Track::Stat::Loved);
    }
    if(const Track::Stats otherStats = stats & ~Track::Stats{Track::Stat::Loved}; otherStats != Track::Stat::None) {
        library->updateTrackStats(tracks, otherStats);
    }
}

WriteRequest saveTrackMetadata(MusicLibrary* library, const TrackList& tracks, const Track::Stats stats)
{
    WriteRequest request = library->writeTrackMetadata(tracks);

    if(stats.testFlag(Track::Stat::Loved)) {
        TrackList sourceTracks;
        std::ranges::copy_if(tracks, std::back_inserter(sourceTracks), [library](const Track& track) {
            return library->trackForId(track.id()).readOnlyStats().testFlag(Track::Stat::Loved);
        });
        if(!sourceTracks.empty()) {
            library->editTrackStats(sourceTracks, Track::Stat::Loved);
        }
    }

    return request;
}

bool canSaveTrackStats(MusicLibrary* library, const TrackList& tracks, const Track::Stats stats)
{
    return !stats.testFlag(Track::Stat::Loved) || std::ranges::all_of(tracks, [library](const Track& track) {
        return library->canEditTrackStats(track, Track::Stat::Loved);
    });
}
} // namespace Fooyin::TagEditor
