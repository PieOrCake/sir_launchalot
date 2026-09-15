// Throwaway harness: does the account config still load old files, and does the
// new per-addon selection survive a save/load round trip?
#include "core/AccountManager.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTextStream>

static int g_pass = 0, g_fail = 0;
static QTextStream out(stdout);

static void check(bool ok, const QString &what)
{
    if (ok) { ++g_pass; out << "  ok    " << what << "\n"; }
    else    { ++g_fail; out << "  FAIL  " << what << "\n"; }
}

static void writeConfig(const QString &dir, const QString &json)
{
    QDir().mkpath(dir);
    QFile f(dir + "/accounts.json");
    if (!f.open(QIODevice::WriteOnly)) return;
    f.write(json.toUtf8());
    f.close();
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir tmp;

    // --- an existing config, written before per-addon selection existed
    out << "\n[migration] config with only the old enableAddons flag\n";
    const QString dirA = tmp.path() + "/a";
    writeConfig(dirA, R"({
        "accounts": [
            {"id":"main","displayName":"Main","isMain":true,"enableAddons":true},
            {"id":"alt1","displayName":"Alt One","isMain":false,"enableAddons":false},
            {"id":"alt2","displayName":"Alt Two","isMain":false,"enableAddons":true}
        ]
    })");

    {
        AccountManager am;
        am.setConfigDir(dirA);
        check(am.load(), "old config loads");
        check(am.account("alt1").addonMode == AccountManager::AddonMode::Off,
              "addons off becomes mode Off");
        check(am.account("alt2").addonMode == AccountManager::AddonMode::All,
              "addons on becomes mode All");
        check(am.account("alt2").addonSelections.isEmpty(),
              "no selections invented for a migrated account");
        check(am.account("alt2").copyNexusLayout,
              "Nexus layout copying defaults to on");
    }

    // --- round trip of the new fields
    out << "\n[round trip] selections survive save and reload\n";
    const QString dirB = tmp.path() + "/b";
    {
        AccountManager am;
        am.setConfigDir(dirB);
        AccountManager::Account acct;
        acct.id = "alt9";
        acct.displayName = "Picky Alt";
        acct.addonMode = AccountManager::AddonMode::Selected;
        acct.copyNexusLayout = false;
        acct.addonSelections = {
            {"PieUI.dll", true, "PieUI"},
            {"TaimiHUD.dll", false, "Taimi"},
            {"rtapi.dll", true, ""}
        };
        check(am.addAccount(acct), "account with selections saves");
    }
    {
        AccountManager am;
        am.setConfigDir(dirB);
        check(am.load(), "config reloads");
        const auto acct = am.account("alt9");
        check(acct.addonMode == AccountManager::AddonMode::Selected, "mode Selected persists");
        check(!acct.copyNexusLayout, "Nexus layout opt-out persists");
        check(acct.addonSelections.size() == 3, "all three selections persist");
        check(acct.addonSelections[0].dll == "PieUI.dll"
                  && acct.addonSelections[0].copySettings
                  && acct.addonSelections[0].dataDir == "PieUI",
              "settings folder and copy flag persist");
        check(!acct.addonSelections[1].copySettings, "per-addon copy opt-out persists");
        check(acct.addonSelections[2].dataDir.isEmpty(), "an addon with no folder persists");
    }

    // --- the compatibility flag an older build would read
    out << "\n[compatibility] enableAddons still written for older builds\n";
    {
        QFile f(dirB + "/accounts.json");
        f.open(QIODevice::ReadOnly);
        const auto doc = QJsonDocument::fromJson(f.readAll());
        f.close();
        const auto obj = doc.object()["accounts"].toArray()[0].toObject();
        check(obj.contains("enableAddons") && obj["enableAddons"].toBool(),
              "Selected mode reports addons enabled to an older build");
        check(obj["addonMode"].toString() == "selected", "addonMode written as a string");
    }

    // --- issue #6: last seen combines the API time with a locally watched session
    out << "\n[last seen] API time vs a session the launcher watched\n";
    {
        const QDateTime now = QDateTime::currentDateTimeUtc();
        const QString apiOld = now.addSecs(-3600).toString(Qt::ISODate);   // an hour ago
        const QString apiNew = now.addSecs(-60).toString(Qt::ISODate);     // a minute ago
        const qint64 playedRecently = now.addSecs(-120).toSecsSinceEpoch(); // two minutes ago

        check(!AccountManager::lastSeen(QString(), 0).isValid(),
              "nothing known means nothing shown");
        check(AccountManager::lastSeen(apiOld, 0).isValid(),
              "the API time is used when there is no local session");
        check(AccountManager::lastSeen(QString(), playedRecently).isValid(),
              "a local session shows even with no API data");

        const QDateTime beatsStaleApi = AccountManager::lastSeen(apiOld, playedRecently);
        check(qAbs(beatsStaleApi.toSecsSinceEpoch() - playedRecently) < 2,
              "a just-finished session wins over an hour-old API time");

        const QDateTime apiWins = AccountManager::lastSeen(apiNew, playedRecently);
        check(qAbs(apiWins.secsTo(now)) < 62 && apiWins > QDateTime::fromSecsSinceEpoch(playedRecently, Qt::UTC),
              "a newer API time wins over an older local session");

        check(!AccountManager::lastSeen("not a date", 0).isValid(),
              "an unparseable API time is ignored");
        check(AccountManager::lastSeen("not a date", playedRecently).isValid(),
              "an unparseable API time still falls back to the local session");
    }

    // --- lastPlayed must survive a save/load round trip
    out << "\n[last seen] the timestamp persists\n";
    {
        const QString dirC = tmp.path() + "/c";
        const qint64 stamp = QDateTime::currentSecsSinceEpoch();
        {
            AccountManager am;
            am.setConfigDir(dirC);
            AccountManager::Account a;
            a.id = "alt7"; a.displayName = "Played"; a.lastPlayed = stamp;
            AccountManager::Account b;
            b.id = "alt8"; b.displayName = "Never";
            check(am.addAccount(a) && am.addAccount(b), "accounts save");
        }
        AccountManager am;
        am.setConfigDir(dirC);
        am.load();
        check(am.account("alt7").lastPlayed == stamp, "lastPlayed round trips exactly");
        check(am.account("alt8").lastPlayed == 0, "an unplayed account stays at zero");
    }

    out << QString("\n%1 passed, %2 failed\n").arg(g_pass).arg(g_fail);
    out.flush();
    return g_fail;
}
