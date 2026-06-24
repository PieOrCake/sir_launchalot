#include "ProtonResolver.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>

ProtonResolver::ProtonResolver(const QStringList &searchDirs, const QString &umuRuntimeBase)
    : m_searchDirs(searchDirs), m_umuRuntimeBase(umuRuntimeBase)
{
}

ProtonResolver ProtonResolver::forHost()
{
    const QString home = QDir::homePath();
    QStringList dirs = {
        home + "/.steam/steam/compatibilitytools.d",
        home + "/.local/share/Steam/compatibilitytools.d",
        home + "/.steam/root/compatibilitytools.d",
        home + "/.local/share/umu/compatibilitytools",
    };
    return ProtonResolver(dirs, home + "/.local/share/umu");
}

QString ProtonResolver::runtimeFolderForAppId(const QString &appId)
{
    if (appId == QStringLiteral("1628350")) return QStringLiteral("steamrt3");
    if (appId == QStringLiteral("4183110")) return QStringLiteral("steamrt4");
    return QString();
}

namespace {

struct Candidate {
    QString name;        // e.g. "GE-Proton10-34"
    QString path;        // full directory path
    QString runtime;     // umu runtime folder name (empty if unknown)
    bool runtimePresent = false;
    int major = -1;      // version key; -1 sorts lowest
    int minor = -1;
};

// Read require_tool_appid from a Proton toolmanifest.vdf. Empty if absent.
QString readRequiredAppId(const QString &manifestPath)
{
    QFile f(manifestPath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return QString();
    const QString text = QString::fromUtf8(f.readAll());
    QRegularExpression re(QStringLiteral("\"require_tool_appid\"\\s*\"(\\d+)\""));
    auto m = re.match(text);
    return m.hasMatch() ? m.captured(1) : QString();
}

// Parse the trailing MAJOR-MINOR from a GE-Proton-style directory name.
void parseVersion(const QString &name, int &major, int &minor)
{
    QRegularExpression re(QStringLiteral("(\\d+)-(\\d+)\\s*$"));
    auto m = re.match(name);
    if (m.hasMatch()) {
        major = m.captured(1).toInt();
        minor = m.captured(2).toInt();
    }
}

bool isNewer(const Candidate &a, const Candidate &b)  // a newer than b?
{
    if (a.major != b.major) return a.major > b.major;
    return a.minor > b.minor;
}

} // namespace

ProtonResolver::Result ProtonResolver::resolve() const
{
    QList<Candidate> candidates;
    QSet<QString> seenNames;  // dedupe Protons that appear in several search dirs

    for (const QString &dir : m_searchDirs) {
        QDir d(dir);
        if (!d.exists()) continue;

        const QStringList entries = d.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString &entry : entries) {
            if (seenNames.contains(entry)) continue;
            const QString base = dir + "/" + entry;

            // Must be a real Proton install. GE-Proton11+ ships only "wine"
            // (no separate wine64), so accept either binary in either layout.
            const bool hasWine = QFile::exists(base + "/files/bin/wine64")
                              || QFile::exists(base + "/files/bin/wine")
                              || QFile::exists(base + "/dist/bin/wine64")
                              || QFile::exists(base + "/dist/bin/wine");
            const QString manifest = base + "/toolmanifest.vdf";
            if (!hasWine || !QFile::exists(manifest)) continue;

            Candidate c;
            c.name = entry;
            c.path = base;
            c.runtime = runtimeFolderForAppId(readRequiredAppId(manifest));
            c.runtimePresent = !c.runtime.isEmpty()
                && QFile::exists(m_umuRuntimeBase + "/" + c.runtime + "/toolmanifest.vdf");
            parseVersion(entry, c.major, c.minor);
            candidates.append(c);
            seenNames.insert(entry);
        }
    }

    if (candidates.isEmpty()) {
        return Result{Status::NoProtonInstalled, {},
                      QStringLiteral("No GE-Proton found; letting umu-launcher pick one."),
                      {}, {}};
    }

    // Best usable, and best overall (for the failure message).
    const Candidate *bestUsable = nullptr;
    const Candidate *bestOverall = nullptr;
    for (const Candidate &c : candidates) {
        if (!bestOverall || isNewer(c, *bestOverall)) bestOverall = &c;
        if (c.runtimePresent && (!bestUsable || isNewer(c, *bestUsable))) bestUsable = &c;
    }

    if (!bestUsable) {
        const QString rt = bestOverall->runtime.isEmpty()
            ? QStringLiteral("a Steam runtime") : bestOverall->runtime;
        return Result{
            Status::AllRuntimesMissing, {}, {},
            QStringLiteral("Proton Runtime Missing"),
            QStringLiteral("%1 needs Steam Runtime '%2', which isn't installed. "
                           "Install an older GE-Proton (e.g. 10.x), or update umu-launcher "
                           "so it can download the runtime.")
                .arg(bestOverall->name, rt)};
    }

    // Note any newer Protons we skipped because their runtime is absent.
    QStringList skipped;
    for (const Candidate &c : candidates) {
        if (!c.runtimePresent && isNewer(c, *bestUsable)) {
            skipped << QStringLiteral("%1 (needs %2)").arg(
                c.name, c.runtime.isEmpty() ? QStringLiteral("unknown runtime") : c.runtime);
        }
    }
    QString log = QStringLiteral("Auto-selected Proton %1 (runtime %2)")
                      .arg(bestUsable->name, bestUsable->runtime);
    if (!skipped.isEmpty())
        log += QStringLiteral("; skipped %1 — runtime not installed").arg(skipped.join(", "));

    return Result{Status::Resolved, bestUsable->path, log, {}, {}};
}
