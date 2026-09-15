// The account context menu captures raw QListWidgetItem pointers, and
// menu.exec() runs a nested event loop in which the list can be rebuilt (an API
// reply arriving, or a game exiting). Choosing "Edit" then acts on a deleted
// item: either a crash, or the wrong account.
//
// Reordering the list before triggering Edit makes the failure visible rather
// than relying on a segfault, which depends on whether the freed memory
// happened to be reused.
#include "core/AccountManager.h"
#include "ui/MainWindow.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>

static QTextStream out(stdout);
static int g_fail = 0;

int main(int argc, char *argv[])
{
    // Run against a throwaway copy of the fixture config. Redirecting
    // XDG_CONFIG_HOME keeps the production path layout while making it
    // impossible to touch the real one, and gives every run a clean config.
    QTemporaryDir configHome;
    if (!configHome.isValid()) { out << "could not create a temp config dir\n"; return 1; }
    const QString appConfig = configHome.path() + "/sir-launchalot/Sir Launchalot";
    if (!QDir().mkpath(appConfig)) { out << "could not create " << appConfig << "\n"; return 1; }
    if (!QFile::copy(QStringLiteral(TEST_FIXTURE_DIR "/accounts.json"),
                     appConfig + "/accounts.json")) {
        out << "could not copy the fixture config\n";
        return 1;
    }
    qputenv("XDG_CONFIG_HOME", configHome.path().toUtf8());

    // The scenario drives a context menu through a nested event loop, which a
    // real desktop's window manager interferes with — on a live session the
    // menu never reports its actions. Force offscreen so running this by hand
    // behaves the same as running it under ctest.
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");

    QApplication app(argc, argv);
    app.setApplicationName("Sir Launchalot");
    app.setOrganizationName("sir-launchalot");

    MainWindow window(false);
    window.show();

    auto *list = window.findChild<QListWidget *>();
    auto *accounts = window.findChild<AccountManager *>();
    if (!list || !accounts) { out << "harness could not find the widgets\n"; return 1; }

    const QString wantedId = "12e5450e";          // Alt One
    int target = -1;
    for (int i = 0; i < list->count(); ++i)
        if (list->item(i)->data(Qt::UserRole).toString() == wantedId) target = i;
    if (target < 0) { out << "alt not found\n"; return 1; }
    out << "right-clicking row " << target << " (Alt One)\n"; out.flush();

    QTimer::singleShot(600, [&]() {
        // Move Alt One to the end, then rebuild — as an API reply would.
        QStringList order = accounts->orderedItemIds();
        order.removeAll(wantedId);
        order.prepend(wantedId);   // always a different row to the one clicked
        accounts->reorderItems(order);
        QMetaObject::invokeMethod(&window, "onInstanceStopped", Q_ARG(QString, wantedId));
        out << "  list rebuilt behind the menu; Alt One is now row "
            << 0 << "\n";
        out.flush();

        QMenu *menu = nullptr;
        for (auto *w : QApplication::topLevelWidgets())
            if (auto *m = qobject_cast<QMenu *>(w)) if (m->isVisible()) menu = m;
        QAction *edit = nullptr;
        if (menu) for (auto *a : menu->actions()) if (a->text() == "Edit") edit = a;
        if (!edit) { out << "  FAIL  no Edit action found\n"; ++g_fail; qApp->quit(); return; }

        // Inspect whichever account dialog Edit opens, then close it. If Edit
        // opens nothing at all — which is what a stale item pointer does when
        // the freed memory is not reused — sawDialog stays false.
        static bool sawDialog = false;
        static QString shownName;
        QTimer::singleShot(500, []() {
            auto *dlg = QApplication::activeModalWidget();
            if (!dlg) return;
            sawDialog = true;
            auto *name = dlg->findChild<QLineEdit *>();
            shownName = name ? name->text() : QString("<no name field>");
            dlg->close();
        });

        out << "  choosing Edit\n"; out.flush();
        edit->trigger();   // blocks while the account dialog is open

        if (!sawDialog) {
            out << "  FAIL  Edit opened nothing — the captured item was stale\n";
            ++g_fail;
        } else if (shownName != "Alt One") {
            out << "  FAIL  Edit opened \"" << shownName << "\", expected \"Alt One\"\n";
            ++g_fail;
        } else {
            out << "  ok    Edit opened the account that was right-clicked\n";
        }
        menu->close();
        QTimer::singleShot(200, []() { qApp->quit(); });
    });

    const QPoint pos = list->visualItemRect(list->item(target)).center();
    QMetaObject::invokeMethod(&window, "onAccountContextMenu", Q_ARG(QPoint, pos));

    app.exec();
    out << (g_fail ? "\nFAILED\n" : "\nPASSED\n");
    out.flush();
    return g_fail;
}
