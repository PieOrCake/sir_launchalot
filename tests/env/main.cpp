// Throwaway harness for issue #9: per-account environment variables were saved
// but never reached the game's launch script.
#include "core/AccountManager.h"
#include "core/ProcessManager.h"

#include <QCoreApplication>
#include <QTextStream>

static int g_pass = 0, g_fail = 0;
static QTextStream out(stdout);

static void check(bool ok, const QString &what)
{
    if (ok) { ++g_pass; out << "  ok    " << what << "\n"; }
    else    { ++g_fail; out << "  FAIL  " << what << "\n"; }
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    out << "\n[empty]\n";
    check(ProcessManager::envExportLines({}).isEmpty(), "no variables produces no output");

    out << "\n[ordinary variables]\n";
    const QString s = ProcessManager::envExportLines({
        {"DXVK_HUD", "fps,memory"},
        {"MANGOHUD", "1"},
    });
    out << s;
    check(s.contains("export DXVK_HUD='fps,memory'\n"), "a variable is exported");
    check(s.contains("export MANGOHUD='1'\n"), "a second variable is exported");
    check(s.contains("# Per-account environment"), "the block is labelled in the script");

    out << "\n[launcher-managed names are refused]\n";
    const QString m = ProcessManager::envExportLines({
        {"WINEPREFIX", "/tmp/evil"},
        {"PROTONPATH", "/tmp/evil"},
        {"GAMEID", "x"},
        {"STORE", "x"},
        {"wineprefix", "/tmp/evil"},
        {"DXVK_HUD", "fps"},
    });
    out << m;
    check(!m.contains("WINEPREFIX"), "WINEPREFIX cannot be overridden");
    check(!m.contains("PROTONPATH"), "PROTONPATH cannot be overridden");
    check(!m.contains("GAMEID"), "GAMEID cannot be overridden");
    check(!m.contains("wineprefix"), "the check is case-insensitive");
    check(m.contains("export DXVK_HUD='fps'"), "other variables still come through");

    out << "\n[awkward values]\n";
    const QString q = ProcessManager::envExportLines({
        {"WITH_QUOTE", "it's here"},
        {"WITH_SPACE", "a b c"},
        {"EMPTY", ""},
        {"  ", "ignored"},
    });
    out << q;
    check(q.contains("export WITH_QUOTE='it'\\''s here'"), "a single quote is escaped for the shell");
    check(q.contains("export WITH_SPACE='a b c'"), "spaces survive");
    check(q.contains("export EMPTY=''"), "an empty value is allowed");
    check(!q.contains("ignored"), "a blank name is skipped");

    // --- NAME=value parsing, as typed or pasted
    out << "\n[parsing] one variable\n";
    {
        QStringList errs;
        auto v = AccountManager::parseEnvAssignments("PULSE_SINK=gw2_seat1", &errs);
        check(v.size() == 1 && v.value("PULSE_SINK") == "gw2_seat1", "a plain assignment");
        check(errs.isEmpty(), "no complaints");
    }

    out << "\n[parsing] a pasted block\n";
    {
        QStringList errs;
        auto v = AccountManager::parseEnvAssignments(
            "# from the wiki\n"
            "export PULSE_SINK=gw2_seat1\n"
            "\n"
            "  DXVK_HUD = fps,memory  \n"
            "WINEDLLOVERRIDES=\"d3d11=n,b\"\n", &errs);
        check(v.size() == 3, QString("three variables (got %1)").arg(v.size()));
        check(v.value("PULSE_SINK") == "gw2_seat1", "a leading export is dropped");
        check(v.value("DXVK_HUD") == "fps,memory", "whitespace around name and value is trimmed");
        check(v.value("WINEDLLOVERRIDES") == "d3d11=n,b",
              "quotes are shell syntax, and a value may contain =");
        check(errs.isEmpty(), "comments and blank lines are not errors");
    }

    out << "\n[parsing] awkward but legal\n";
    {
        QStringList errs;
        auto v = AccountManager::parseEnvAssignments("EMPTY=\nA_1=x\n", &errs);
        check(v.contains("EMPTY") && v.value("EMPTY").isEmpty(), "an empty value is allowed");
        check(v.contains("A_1"), "digits and underscores are allowed in a name");
        check(errs.isEmpty(), "no complaints");
    }

    out << "\n[parsing] rejected, with a reason\n";
    {
        QStringList errs;
        auto v = AccountManager::parseEnvAssignments("JUST_A_NAME", &errs);
        check(v.isEmpty() && errs.size() == 1, "a line with no = is refused");
        check(errs.first().contains("NAME=value"), "and says what was expected");
    }
    {
        QStringList errs;
        auto v = AccountManager::parseEnvAssignments("=orphan", &errs);
        check(v.isEmpty() && errs.size() == 1, "a missing name is refused");
    }
    {
        QStringList errs;
        auto v = AccountManager::parseEnvAssignments("1BAD=x\nHAS SPACE=y\nHAS-DASH=z", &errs);
        check(v.isEmpty(), "names that a shell could not export are refused");
        check(errs.size() == 3, QString("each bad name is reported (got %1)").arg(errs.size()));
    }
    {
        QStringList errs;
        auto v = AccountManager::parseEnvAssignments("WINEPREFIX=/tmp/evil\nOK=1", &errs);
        check(!v.contains("WINEPREFIX"), "a launcher-managed name is refused here too");
        check(v.value("OK") == "1", "and the rest of the block still parses");
        check(errs.size() == 1 && errs.first().contains("Wine prefix"),
              "the refusal explains why");
    }

    out << QString("\n%1 passed, %2 failed\n").arg(g_pass).arg(g_fail);
    out.flush();
    return g_fail;
}
