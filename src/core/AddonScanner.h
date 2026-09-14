#ifndef ADDONSCANNER_H
#define ADDONSCANNER_H

#include <QList>
#include <QString>
#include <QStringList>

// Reads a Raidcore Nexus addons folder and works out, for each addon DLL, which
// settings folder belongs to it.
//
// Nexus scans <game>/addons/ non-recursively and loads every *.dll it finds, so
// the DLL list is the authoritative list of installed addons. An addon's settings
// folder name, however, is whatever string the addon passes to the Nexus API —
// it is not derived from the file name (TaimiHUD.dll uses addons/Taimi/). The
// detection here combines a relaxed name match with a scan of the DLL itself for
// the folder name, which the addon has to carry as a string literal.
class AddonScanner
{
public:
    struct AddonInfo {
        QString dll;          // file name, e.g. "Radial_Menus.dll"
        QString displayName;  // "Radial Menus"
        QString dataDir;      // settings folder name, empty when undetected
        qint64  dataSize = -1;  // bytes on disk, -1 until computed
    };

    // Folders inside addons/ that belong to Nexus rather than to any one addon.
    static const QStringList &reservedDirs();

    // Every *.dll in addonsDir, sorted by display name, with dataDir filled in
    // where it could be determined. dataSize is left at -1; it is expensive
    // enough to be worth computing separately.
    static QList<AddonInfo> scan(const QString &addonsDir);

    // Candidate settings folders a user can choose from, i.e. every subdirectory
    // of addonsDir except the reserved ones.
    static QStringList candidateDataDirs(const QString &addonsDir);

    // The settings folder for dllName, or an empty string when no confident
    // match exists. candidateDirs is the output of candidateDataDirs().
    static QString detectDataDir(const QString &addonsDir, const QString &dllName,
                                 const QStringList &candidateDirs);

    static QString prettyName(const QString &dllName);

    // Total size of a directory tree, in bytes. 0 when it does not exist.
    static qint64 directorySize(const QString &path);

    static QString formatSize(qint64 bytes);

private:
    // The folder whose name matches dllName outright, ignoring case and
    // punctuation. Empty when there is no match or more than one.
    static QString exactDataDir(const QString &dllName, const QStringList &candidateDirs);

    // Lowercase, stripped of anything that is not a letter or digit, so that
    // "Radial_Menus", "RadialMenus" and "radial menus" all compare equal.
    static QString normalise(const QString &s);

    // Folder names that occur as a raw byte sequence inside the DLL.
    static QStringList namesInBinary(const QString &dllPath, const QStringList &names);
};

#endif // ADDONSCANNER_H
