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
} // namespace Fooyin::TagEditor
