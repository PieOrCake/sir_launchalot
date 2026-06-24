#ifndef PROTONRESOLVER_H
#define PROTONRESOLVER_H

#include <QString>
#include <QStringList>

// Decides which Proton to use when an account's Proton is set to "auto".
//
// Picks the newest installed GE-Proton whose required Steam runtime is actually
// present under the umu runtime base, so umu is never handed a Proton whose
// runtime it would crash trying to open.
//
// Pure logic with injectable paths so it can be exercised against fixtures.
class ProtonResolver
{
public:
    enum class Status {
        Resolved,            // protonPath holds the chosen Proton directory
        NoProtonInstalled,   // no candidate Proton dirs found at all
        AllRuntimesMissing,  // Protons exist but none have an installed runtime
    };

    struct Result {
        Status status;
        QString protonPath;     // set when Resolved
        QString logMessage;     // human-readable: what was picked / skipped & why
        QString blockTitle;     // set when AllRuntimesMissing
        QString blockMessage;   // set when AllRuntimesMissing
    };

    // searchDirs: directories that may contain Proton installs.
    // umuRuntimeBase: base dir holding steamrt3 / steamrt4 / ... runtime folders.
    ProtonResolver(const QStringList &searchDirs, const QString &umuRuntimeBase);

    // Build a resolver wired to the standard host paths.
    static ProtonResolver forHost();

    Result resolve() const;

    // Maps a Proton toolmanifest require_tool_appid to its umu runtime folder
    // name. Returns an empty string for unknown appids.
    static QString runtimeFolderForAppId(const QString &appId);

private:
    QStringList m_searchDirs;
    QString m_umuRuntimeBase;
};

#endif // PROTONRESOLVER_H
