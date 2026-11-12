/*
 * Fooyin
 * Copyright © 2026, Nikita Butenko
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <core/accountstore.h>
#include <utils/database/dbconnectionhandler.h>
#include <utils/database/dbconnectionprovider.h>
#include <utils/database/dbquery.h>

#include <QLoggingCategory>
#include <QCoreApplication>
#include <QStandardPaths>
#include <QStringList>
#include <QTemporaryDir>

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using namespace Fooyin;

TEST(AccountStoreTest, FailedWritesRetainTheAccountWithoutLoggingCredentials)
{
    int argc{1};
    char applicationName[]{"accountstore-test"};
    char* argv[]{applicationName, nullptr};
    QCoreApplication application{argc, argv};
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    qputenv("XDG_CONFIG_HOME", directory.path().toUtf8());
    DbConnection::DbParams params;
    params.type = u"QSQLITE"_s;
    params.filePath = directory.filePath(u"fooyin.db"_s);
    const auto dbPool = DbConnectionPool::create(params, u"account-store-test"_s);
    ASSERT_TRUE(dbPool);
    const DbConnectionHandler connection{dbPool};
    ASSERT_TRUE(connection.hasConnection());
    const DbConnectionProvider provider{dbPool};
    DbQuery createSettings{provider.db(), u"CREATE TABLE Settings (Name TEXT PRIMARY KEY, Value TEXT)"_s};
    ASSERT_TRUE(createSettings.exec());
    const AccountStore accountStore{dbPool};
    ASSERT_TRUE(accountStore.save(u"Synthetic"_s, {{u"AccessToken"_s, u"initial-token"_s}}));
    DbQuery rejectCredential{provider.db(), u"CREATE TRIGGER reject_credential BEFORE INSERT ON Settings "
                                           "BEGIN SELECT RAISE(ABORT, 'synthetic refusal'); END"_s};
    ASSERT_TRUE(rejectCredential.exec());
    static QStringList messages;
    messages.clear();
    const auto previousHandler = qInstallMessageHandler([](QtMsgType, const QMessageLogContext&, const QString& message) {
        messages.append(message);
    });
    const auto saved = accountStore.save(u"Synthetic"_s, {{u"AccessToken"_s, u"must-not-enter-logs"_s}});
    qInstallMessageHandler(previousHandler);
    EXPECT_FALSE(saved);
    EXPECT_FALSE(messages.join(u'\n').contains(u"must-not-enter-logs"_s));
    const auto retained = accountStore.load(u"Synthetic"_s, {u"AccessToken"_s});
    ASSERT_TRUE(retained);
    EXPECT_EQ(retained->at(u"AccessToken"_s), u"initial-token"_s);
}
