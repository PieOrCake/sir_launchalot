// Throwaway harness for issue #8: GE-Proton folders carrying an -x86_64 suffix
// were parsed as having no version and lost every comparison.
#include "core/ProtonResolver.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTextStream>

static int g_pass = 0, g_fail = 0;
static QTextStream out(stdout);

static void check(bool ok, const QString &what)
{
    if (ok) { ++g_pass; out << "  ok    " << what << "\n"; }
    else    { ++g_fail; out << "  FAIL  " << what << "\n"; }
}

static void write(const QString &path, const QString &text)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return;
    f.write(text.toUtf8());
    f.close();
}

// A Proton install as the resolver expects to find one.
static void makeProton(const QString &dir, const QString &name, const QString &appId)
{
    const QString base = dir + "/" + name;
    write(base + "/files/bin/wine", "#!/bin/sh\n");
    write(base + "/toolmanifest.vdf",
          QString("\"manifest\"\n{\n  \"require_tool_appid\" \"%1\"\n}\n").arg(appId));
}

static void makeRuntime(const QString &base, const QString &folder)
{
    write(base + "/" + folder + "/toolmanifest.vdf", "\"manifest\"\n{\n}\n");
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir tmp;

    const QString protons = tmp.path() + "/compatibilitytools.d";
    const QString runtimes = tmp.path() + "/umu";

    // The reporter's actual set, plus the runtimes both generations need.
    makeProton(protons, "GE-Proton10-34",        "1628350");  // steamrt3
    makeProton(protons, "GE-Proton11-1",         "1628350");
    makeProton(protons, "GE-Proton11-3",         "1628350");
    makeProton(protons, "GE-Proton11-5-x86_64",  "1628350");
    makeProton(protons, "GE-Proton11-6-x86_64",  "1628350");
    makeRuntime(runtimes, "steamrt3");

    ProtonResolver resolver({protons}, runtimes);
    const auto result = resolver.resolve();

    out << "\n[issue #8] newest of a mixed-naming set\n";
    out << "  picked: " << QFileInfo(result.protonPath).fileName() << "\n";
    check(result.status == ProtonResolver::Status::Resolved, "a Proton was resolved");
    check(QFileInfo(result.protonPath).fileName() == "GE-Proton11-6-x86_64",
          "the suffixed 11-6 wins, not the unsuffixed 11-3");

    // Ordering within the suffixed generation, including the 10-vs-2 digit trap.
    {
        QTemporaryDir t2;
        const QString p = t2.path() + "/p", r = t2.path() + "/r";
        makeProton(p, "GE-Proton11-2-x86_64", "1628350");
        makeProton(p, "GE-Proton11-10-x86_64", "1628350");
        makeRuntime(r, "steamrt3");
        const auto res = ProtonResolver({p}, r).resolve();
        out << "\n[ordering] picked: " << QFileInfo(res.protonPath).fileName() << "\n";
        check(QFileInfo(res.protonPath).fileName() == "GE-Proton11-10-x86_64",
              "11-10 beats 11-2 (compared as numbers, not text)");
    }

    // The old naming must still behave exactly as before.
    {
        QTemporaryDir t3;
        const QString p = t3.path() + "/p", r = t3.path() + "/r";
        makeProton(p, "GE-Proton9-27", "1628350");
        makeProton(p, "GE-Proton10-34", "1628350");
        makeRuntime(r, "steamrt3");
        const auto res = ProtonResolver({p}, r).resolve();
        out << "\n[unsuffixed] picked: " << QFileInfo(res.protonPath).fileName() << "\n";
        check(QFileInfo(res.protonPath).fileName() == "GE-Proton10-34",
              "10-34 still beats 9-27");
    }

    // A suffixed Proton whose runtime is missing must still be skipped, not
    // picked just because it now parses as the newest.
    {
        QTemporaryDir t4;
        const QString p = t4.path() + "/p", r = t4.path() + "/r";
        makeProton(p, "GE-Proton11-3", "1628350");           // steamrt3, present
        makeProton(p, "GE-Proton11-6-x86_64", "4183110");    // steamrt4, absent
        makeRuntime(r, "steamrt3");
        const auto res = ProtonResolver({p}, r).resolve();
        out << "\n[runtime gate] picked: " << QFileInfo(res.protonPath).fileName() << "\n";
        check(QFileInfo(res.protonPath).fileName() == "GE-Proton11-3",
              "newest is skipped when its runtime is not installed");
    }

    out << QString("\n%1 passed, %2 failed\n").arg(g_pass).arg(g_fail);
    out.flush();
    return g_fail;
}
