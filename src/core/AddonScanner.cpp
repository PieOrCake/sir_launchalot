#include "core/AddonScanner.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QLocale>

const QStringList &AddonScanner::reservedDirs()
{
    // Nexus's own folder, the shared HTTP cache and the crash/log dump.
    static const QStringList dirs = {"Nexus", "common", "logs"};
    return dirs;
}

QString AddonScanner::normalise(const QString &s)
{
    QString out;
    out.reserve(s.size());
    for (const QChar &c : s) {
        if (c.isLetterOrNumber())
            out.append(c.toLower());
    }
    return out;
}

QString AddonScanner::prettyName(const QString &dllName)
{
    QString stem = dllName;
    if (stem.endsWith(".dll", Qt::CaseInsensitive))
        stem.chop(4);
    stem.replace('_', ' ');
    return stem.simplified();
}

QStringList AddonScanner::candidateDataDirs(const QString &addonsDir)
{
    QStringList dirs = QDir(addonsDir).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &reserved : reservedDirs())
        dirs.removeAll(reserved);
    return dirs;
}

QStringList AddonScanner::namesInBinary(const QString &dllPath, const QStringList &names)
{
    QFile f(dllPath);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    const QByteArray data = f.readAll();
    f.close();

    QStringList hits;
    for (const QString &name : names) {
        // Two-character names produce far too many accidental matches to be
        // worth anything as evidence.
        if (name.size() < 3) continue;
        if (data.contains(name.toUtf8()))
            hits.append(name);
    }
    return hits;
}

QString AddonScanner::detectDataDir(const QString &addonsDir, const QString &dllName,
                                    const QStringList &candidateDirs)
{
    QString stem = dllName;
    if (stem.endsWith(".dll", Qt::CaseInsensitive))
        stem.chop(4);
    const QString key = normalise(stem);
    if (key.isEmpty()) return {};

    QStringList byName;
    for (const QString &dir : candidateDirs) {
        if (normalise(dir) == key)
            byName.append(dir);
    }

    // A single unambiguous name match is trusted on its own, which keeps the
    // binary scan (and its false positives) out of the common case.
    if (byName.size() == 1)
        return byName.first();

    const QStringList inBinary = namesInBinary(addonsDir + "/" + dllName, candidateDirs);

    // Two folders differing only in punctuation or case — "HorsePocket" and
    // "horse_pocket". Whichever the DLL actually names is the live one.
    if (byName.size() > 1) {
        QStringList both;
        for (const QString &dir : byName) {
            if (inBinary.contains(dir))
                both.append(dir);
        }
        if (both.size() == 1)
            return both.first();
        if (byName.contains(stem))
            return stem;
        return {};
    }

    // No name match at all: the folder is named differently to the DLL. Accept a
    // string found in the binary only when it also overlaps the file name at one
    // end — "Taimi" for TaimiHUD.dll, "Chat" for Events_Chat.dll — otherwise
    // short words like "arcdps" match half the addons in the folder.
    QStringList related;
    for (const QString &dir : inBinary) {
        const QString n = normalise(dir);
        if (n.isEmpty()) continue;
        if (key.startsWith(n) || key.endsWith(n) || n.startsWith(key) || n.endsWith(key))
            related.append(dir);
    }
    if (related.size() == 1)
        return related.first();

    return {};
}

QList<AddonScanner::AddonInfo> AddonScanner::scan(const QString &addonsDir)
{
    QList<AddonInfo> out;
    QDir dir(addonsDir);
    if (!dir.exists()) return out;

    const QStringList candidates = candidateDataDirs(addonsDir);
    const QStringList dlls = dir.entryList({"*.dll"}, QDir::Files, QDir::Name);

    for (const QString &dll : dlls) {
        AddonInfo info;
        info.dll = dll;
        info.displayName = prettyName(dll);
        info.dataDir = detectDataDir(addonsDir, dll, candidates);
        out.append(info);
    }

    std::sort(out.begin(), out.end(), [](const AddonInfo &a, const AddonInfo &b) {
        return a.displayName.compare(b.displayName, Qt::CaseInsensitive) < 0;
    });
    return out;
}

qint64 AddonScanner::directorySize(const QString &path)
{
    if (path.isEmpty() || !QDir(path).exists()) return 0;

    qint64 total = 0;
    QDirIterator it(path, QDir::Files | QDir::NoSymLinks,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        total += it.fileInfo().size();
    }
    return total;
}

QString AddonScanner::formatSize(qint64 bytes)
{
    if (bytes <= 0) return QStringLiteral("—");
    return QLocale().formattedDataSize(bytes, 1, QLocale::DataSizeTraditionalFormat);
}
