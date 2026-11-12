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

#pragma once

#include "lyrics.h"

#include <core/network/networkaccessmanager.h>
#include <gui/propertiesdialog.h>

namespace Fooyin {
class PlayerController;

namespace Lyrics {
class LyricsEditor;
class LyricsFinder;
class LyricsSaver;

class LyricsPropertiesTab : public PropertiesTabWidget
{
    Q_OBJECT

public:
    using TrackEditable = std::function<bool(const Track&)>;

    LyricsPropertiesTab(const Track& track, std::shared_ptr<NetworkAccessManager> networkAccess,
                        LyricsSaver* lyricsSaver, PlayerController* playerController, SettingsManager* settings,
                        TrackEditable canEditTrack = {}, QWidget* parent = nullptr);

    void setTrackScope(const TrackList& tracks) override;
    [[nodiscard]] bool isAvailableForScope(const TrackList& tracks) const override;
    [[nodiscard]] bool hasPendingScopeChanges() const override;

    bool commitPendingChanges() override;
    bool apply() override;
    void finish() override;

    [[nodiscard]] QSize sizeHint() const override;

Q_SIGNALS:
    void lyricsEdited(const Fooyin::Lyrics::Lyrics& lyrics);

private:
    struct Draft
    {
        Track originalTagTrack;
        Lyrics originalLyrics;
        Lyrics workingLyrics;
        bool originalLyricsLoaded{false};
        bool dirty{false};
    };

    static QString trackKey(const Track& track);
    void updateTrack(const Track& track);
    void updateLyrics(const Track& track, const Lyrics& lyrics);

    [[nodiscard]] Draft& ensureDraft(const Track& track);
    [[nodiscard]] Draft* currentDraft();
    [[nodiscard]] const Draft* currentDraft() const;

    void loadCurrentDraft();
    void storeCurrentDraftText();
    void updatePendingState();
    void setupConnections();

    void reset();

    Track m_track;
    std::shared_ptr<NetworkAccessManager> m_networkAccess;
    LyricsSaver* m_lyricsSaver;
    PlayerController* m_playerController;
    SettingsManager* m_settings;
    TrackEditable m_canEditTrack;

    LyricsFinder* m_lyricsFinder;
    LyricsEditor* m_editor;

    std::unordered_map<QString, Draft> m_drafts;
    std::unordered_map<QString, Track> m_pendingTagTracks;
    QString m_currentTrackKey;
    bool m_hasPendingScopeChanges;
};
} // namespace Lyrics
} // namespace Fooyin
