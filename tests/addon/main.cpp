// Throwaway harness for AddonScanner detection. Two modes:
//   fixture  - synthetic addons tree covering the awkward cases (writes to a temp dir)
//   real     - runs against the live addons folder, read-only, prints the mapping
#include "core/AddonScanner.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTextStream>

static int g_pass = 0, g_fail = 0;
static QTextStream out(stdout);

static void check(bool ok, const QString &what)
{
    if (ok) { ++g_pass; out << "  ok    " << what << "\n"; }
    else    { ++g_fail; out << "  FAIL  " << what << "\n"; }
}

static void eq(const QString &got, const QString &want, const QString &what)
{
    check(got == want, QString("%1  (got '%2', want '%3')").arg(what, got, want));
}

// Writes a fake DLL whose bytes contain the given string literals, the way a real
// addon carries the name it hands to the Nexus API.
static void writeDll(const QString &path, const QStringList &literals)
{
    QFile f(path);
    f.open(QIODevice::WriteOnly);
    f.write(QByteArray("MZ\x90\x00", 4));
    f.write(QByteArray(2048, 'x'));
    for (const QString &lit : literals) {
        f.write(lit.toUtf8());
        f.write(QByteArray(1, '\0'));
        f.write(QByteArray(64, 'y'));
    }
    f.close();
}

static int runFixture()
{
    QTemporaryDir tmp;
    const QString A = tmp.path() + "/addons";
    QDir().mkpath(A);

    auto mkdir = [&](const QString &d) { QDir().mkpath(A + "/" + d); };

    // --- exact name match, nothing else needed
    writeDll(A + "/PieUI.dll", {"PieUI"});
    mkdir("PieUI");

    // --- name matches once punctuation and case are ignored
    writeDll(A + "/Radial_Menus.dll", {"RadialMenus"});
    mkdir("RadialMenus");

    // --- two folders that both normalise to the DLL name; the binary decides
    writeDll(A + "/HorsePocket.dll", {"HorsePocket"});
    mkdir("HorsePocket");
    mkdir("horse_pocket");

    // --- folder name is a prefix of the DLL name, found only in the binary
    writeDll(A + "/TaimiHUD.dll", {"Taimi", "arcdps", "Chat"});
    mkdir("Taimi");
    mkdir("arcdps");
    mkdir("Chat");

    // --- folder name is a suffix of the DLL name
    writeDll(A + "/Events_Chat.dll", {"Chat"});

    // --- no folder at all: an addon that stores nothing
    writeDll(A + "/rtapi.dll", {"nothing here"});

    // --- binary mentions an unrelated folder; must not be adopted
    writeDll(A + "/Events_Alerts.dll", {"arcdps"});

    // --- reserved folders are never offered
    mkdir("Nexus");
    mkdir("common");
    mkdir("logs");

    const QStringList cands = AddonScanner::candidateDataDirs(A);
    out << "\n[fixture] candidate folders: " << cands.join(", ") << "\n";
    check(!cands.contains("Nexus"), "Nexus is not a candidate folder");
    check(!cands.contains("common"), "common is not a candidate folder");
    check(!cands.contains("logs"), "logs is not a candidate folder");

    auto detect = [&](const QString &dll) {
        return AddonScanner::detectDataDir(A, dll, cands);
    };

    out << "[fixture] detection\n";
    eq(detect("PieUI.dll"), "PieUI", "exact match");
    eq(detect("Radial_Menus.dll"), "RadialMenus", "underscores ignored");
    eq(detect("HorsePocket.dll"), "HorsePocket", "ambiguous pair resolved by binary");
    eq(detect("TaimiHUD.dll"), "Taimi", "prefix match via binary");
    eq(detect("Events_Chat.dll"), "Chat", "suffix match via binary");
    eq(detect("rtapi.dll"), "", "no folder means no guess");
    eq(detect("Events_Alerts.dll"), "", "unrelated binary hit is rejected");

    out << "[fixture] listing\n";
    const auto list = AddonScanner::scan(A);
    check(list.size() == 7, QString("scan found 7 addons (got %1)").arg(list.size()));
    check(list.first().displayName == "Events Alerts",
          "sorted by display name, underscores shown as spaces");

    // --- regression: one addon's folder must never be handed to another addon.
    // SerenadeSpike.dll was installed before its own folder existed, so the
    // prefix rule that maps TaimiHUD.dll -> Taimi/ handed it Serenade.dll's
    // folder instead, and the alt was seeded with the wrong addon's settings.
    out << "[fixture] a folder already owned by another addon\n";
    {
        QTemporaryDir t;
        const QString A2 = t.path() + "/addons";
        QDir().mkpath(A2);
        writeDll(A2 + "/Serenade.dll", {"Serenade"});
        QDir().mkpath(A2 + "/Serenade");
        writeDll(A2 + "/SerenadeSpike.dll", {"SerenadeSpike", "Serenade"});
        // SerenadeSpike has not run yet, so it has no folder of its own.

        auto byDll = [](const QList<AddonScanner::AddonInfo> &l, const QString &dll) {
            for (const auto &a : l) if (a.dll == dll) return a.dataDir;
            return QString("<missing>");
        };
        const auto before = AddonScanner::scan(A2);
        eq(byDll(before, "Serenade.dll"), "Serenade", "the owner keeps its folder");
        eq(byDll(before, "SerenadeSpike.dll"), "",
           "no folder is guessed when the only candidate belongs to another addon");

        // Once the addon has run and made its own folder, it is matched exactly.
        QDir().mkpath(A2 + "/SerenadeSpike");
        const auto after = AddonScanner::scan(A2);
        eq(byDll(after, "Serenade.dll"), "Serenade", "the owner still keeps its folder");
        eq(byDll(after, "SerenadeSpike.dll"), "SerenadeSpike",
           "the addon picks up its own folder once it exists");
    }

    out << "[fixture] size\n";
    QFile big(A + "/PieUI/blob.bin");
    big.open(QIODevice::WriteOnly);
    big.write(QByteArray(5000, 'z'));
    big.close();
    check(AddonScanner::directorySize(A + "/PieUI") == 5000, "directorySize counts file bytes");
    check(AddonScanner::directorySize(A + "/does-not-exist") == 0, "missing folder is 0 bytes");

    return g_fail;
}

static int runReal()
{
    const QString A = QDir::homePath() + "/Games/gw2/addons";
    if (!QDir(A).exists()) {
        out << "no addons folder at " << A << "\n";
        return 1;
    }
    const auto list = AddonScanner::scan(A);
    out << "\n" << list.size() << " addons in " << A << "\n\n";
    int resolved = 0;
    for (const auto &a : list) {
        const qint64 sz = a.dataDir.isEmpty() ? 0 : AddonScanner::directorySize(A + "/" + a.dataDir);
        if (!a.dataDir.isEmpty()) ++resolved;
        out << QString("  %1 %2 %3\n")
                   .arg(a.displayName, -26)
                   .arg(a.dataDir.isEmpty() ? QString("(none detected)") : a.dataDir, -24)
                   .arg(a.dataDir.isEmpty() ? QString() : AddonScanner::formatSize(sz));
    }
    out << QString("\nresolved %1 of %2\n").arg(resolved).arg(list.size());
    return 0;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QString mode = argc > 1 ? QString::fromLatin1(argv[1]) : QStringLiteral("fixture");

    int rc = (mode == "real") ? runReal() : runFixture();

    if (mode != "real")
        out << QString("\n%1 passed, %2 failed\n").arg(g_pass).arg(g_fail);
    out.flush();
    return rc;
}
