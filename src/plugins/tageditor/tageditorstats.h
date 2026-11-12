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

#pragma once

#include <core/track.h>

namespace Fooyin {
class MusicLibrary;
struct WriteRequest;

namespace TagEditor {
//! Saves edited @p stats of @p tracks. Loved uses the native edit path, so an external library can own it.
void saveTrackStats(MusicLibrary* library, const TrackList& tracks, Track::Stats stats);
//! Writes the metadata of @p tracks, and sends their edited Loved value to the external libraries which own it.
WriteRequest saveTrackMetadata(MusicLibrary* library, const TrackList& tracks, Track::Stats stats);
//! Returns @c true when saveTrackStats() can change @p stats of every track in @p tracks.
[[nodiscard]] bool canSaveTrackStats(MusicLibrary* library, const TrackList& tracks, Track::Stats stats);
} // namespace TagEditor
} // namespace Fooyin
