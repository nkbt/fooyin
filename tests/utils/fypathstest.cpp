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

#include <utils/fypaths.h>

#include <QCoreApplication>
#include <QFileInfo>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;

namespace Fooyin::Testing {
TEST(FyPathsTest, UsesExplicitDataAndCacheDirectoriesInTestMode)
{
    int argc{1};
    char applicationName[]{"fypathstest"};
    char* argv[]{applicationName, nullptr};
    QCoreApplication application{argc, argv};
    QTemporaryDir profile;
    ASSERT_TRUE(profile.isValid());
    const auto XDG_DATA_HOME = qgetenv("XDG_DATA_HOME");
    const auto XDG_CACHE_HOME = qgetenv("XDG_CACHE_HOME");
    const bool testMode = QStandardPaths::isTestModeEnabled();
    const auto cleanup = qScopeGuard([&]() {
        qputenv("XDG_DATA_HOME", XDG_DATA_HOME);
        qputenv("XDG_CACHE_HOME", XDG_CACHE_HOME);
        QStandardPaths::setTestModeEnabled(testMode);
    });
    QStandardPaths::setTestModeEnabled(true);
    qputenv("XDG_DATA_HOME", profile.filePath(u"data"_s).toUtf8());
    qputenv("XDG_CACHE_HOME", profile.filePath(u"cache"_s).toUtf8());

    EXPECT_EQ(Utils::sharePath(), profile.filePath(u"data"_s));
    EXPECT_EQ(Utils::cachePath(), profile.filePath(u"cache"_s));
    EXPECT_TRUE(QFileInfo{profile.filePath(u"data"_s)}.isDir());
    EXPECT_TRUE(QFileInfo{profile.filePath(u"cache"_s)}.isDir());
}
} // namespace Fooyin::Testing
