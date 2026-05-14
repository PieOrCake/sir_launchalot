# Sidecar Programs Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Allow any account to launch auxiliary Windows executables (sidecars) inside the same Wine prefix alongside GW2, started before GW2 and killed when GW2 exits.

**Architecture:** `SidecarProgram` records stored per-account in JSON. `AccountDialog` gains a list editor. `ProcessManager` launches each sidecar via `umu-run` before GW2 starts, tracks them in `m_sidecars`, and kills them on stop or natural exit.

**Tech Stack:** C++17, Qt6 (Widgets, Core), umu-run, existing ProcessManager/AccountManager/AccountDialog patterns.

---

## Files

| File | Change |
|------|--------|
| `src/core/AccountManager.h` | Add `SidecarProgram` struct; add `sidecars` field to `Account` |
| `src/core/AccountManager.cpp` | Serialize/deserialize `sidecars` in `accountToJson`/`accountFromJson` |
| `src/ui/AccountDialog.h` | Add table widget members and sidecar list member |
| `src/ui/AccountDialog.cpp` | Add sidecar group box + inline edit dialog; update `setAccount`/`account()` |
| `src/core/ProcessManager.h` | Add `m_sidecars` map; declare three private helpers |
| `src/core/ProcessManager.cpp` | Implement helpers; call them in `launchAccount` and `stopAccount` |

---

### Task 1: Add SidecarProgram struct and field to AccountManager

**Files:**
- Modify: `src/core/AccountManager.h`
- Modify: `src/core/AccountManager.cpp`

- [ ] **Step 1: Add SidecarProgram struct to AccountManager.h**

  Inside the `AccountManager` class, after the `ExternalApp` struct, add:

  ```cpp
  struct SidecarProgram {
      QString id;
      QString name;
      QString exePath;   // Windows-style path, e.g. C:\tools\bridge.exe
      QStringList args;
  };
  ```

  Then in the `Account` struct, after `QStringList extraArgs;`, add:

  ```cpp
  QList<SidecarProgram> sidecars;
  ```

- [ ] **Step 2: Serialize sidecars in accountToJson (AccountManager.cpp)**

  In `accountToJson`, after the `envVars` block and before `return obj;`, add:

  ```cpp
  QJsonArray sidecarsArr;
  for (const auto &sc : account.sidecars) {
      QJsonObject scObj;
      scObj["id"] = sc.id;
      scObj["name"] = sc.name;
      scObj["exePath"] = sc.exePath;
      QJsonArray scArgs;
      for (const auto &a : sc.args) scArgs.append(a);
      scObj["args"] = scArgs;
      sidecarsArr.append(scObj);
  }
  obj["sidecars"] = sidecarsArr;
  ```

- [ ] **Step 3: Deserialize sidecars in accountFromJson (AccountManager.cpp)**

  In `accountFromJson`, after the `envVars` block and before `acct.apiKey = ...`, add:

  ```cpp
  QJsonArray sidecarsArr = obj.value("sidecars").toArray();
  for (const auto &val : sidecarsArr) {
      QJsonObject scObj = val.toObject();
      SidecarProgram sc;
      sc.id = scObj.value("id").toString();
      sc.name = scObj.value("name").toString();
      sc.exePath = scObj.value("exePath").toString();
      QJsonArray scArgs = scObj.value("args").toArray();
      for (const auto &a : scArgs) sc.args.append(a.toString());
      acct.sidecars.append(sc);
  }
  ```

- [ ] **Step 4: Build**

  ```bash
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && make -C build -j$(nproc) 2>&1 | tail -5
  ```

  Expected: build succeeds, no errors.

- [ ] **Step 5: Commit**

  ```bash
  git add src/core/AccountManager.h src/core/AccountManager.cpp
  git commit -m "Add SidecarProgram struct and JSON serialization to AccountManager"
  ```

---

### Task 2: Add sidecar UI to AccountDialog

**Files:**
- Modify: `src/ui/AccountDialog.h`
- Modify: `src/ui/AccountDialog.cpp`

- [ ] **Step 1: Add members to AccountDialog.h**

  Add `#include <QList>` and `#include <QTableWidget>` to the includes in AccountDialog.h.

  In the private section, after `QCheckBox *m_showWeeklyVaultCheck;`, add:

  ```cpp
  QTableWidget *m_sidecarTable;
  QList<AccountManager::SidecarProgram> m_sidecars;
  void refreshSidecarTable();
  ```

- [ ] **Step 2: Add sidecar group box to setupUi in AccountDialog.cpp**

  Add `#include <QPushButton>`, `#include <QHBoxLayout>`, `#include <QTableWidget>`, `#include <QHeaderView>` to the includes at the top of AccountDialog.cpp.

  In `setupUi()`, before the `auto *buttons = new QDialogButtonBox(...)` line, add:

  ```cpp
  auto *sidecarGroup = new QGroupBox("Sidecar Programs");
  auto *sidecarLayout = new QVBoxLayout(sidecarGroup);

  m_sidecarTable = new QTableWidget(0, 2);
  m_sidecarTable->setHorizontalHeaderLabels({"Name", "Exe Path"});
  m_sidecarTable->horizontalHeader()->setStretchLastSection(true);
  m_sidecarTable->setSelectionBehavior(QAbstractItemView::SelectRows);
  m_sidecarTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
  m_sidecarTable->setMinimumHeight(100);
  sidecarLayout->addWidget(m_sidecarTable);

  auto *scBtnLayout = new QHBoxLayout;
  auto *scAddBtn = new QPushButton("Add");
  auto *scEditBtn = new QPushButton("Edit");
  auto *scRemoveBtn = new QPushButton("Remove");
  scBtnLayout->addWidget(scAddBtn);
  scBtnLayout->addWidget(scEditBtn);
  scBtnLayout->addWidget(scRemoveBtn);
  scBtnLayout->addStretch();
  sidecarLayout->addLayout(scBtnLayout);
  layout->addWidget(sidecarGroup);
  ```

- [ ] **Step 3: Implement the inline edit dialog and button connections in AccountDialog.cpp**

  After the `setupUi` function body (before or after other methods), add this helper function and wire up the buttons inside `setupUi` after the button declarations:

  First, add the `refreshSidecarTable` implementation:

  ```cpp
  void AccountDialog::refreshSidecarTable()
  {
      m_sidecarTable->setRowCount(m_sidecars.size());
      for (int i = 0; i < m_sidecars.size(); ++i) {
          m_sidecarTable->setItem(i, 0, new QTableWidgetItem(m_sidecars[i].name));
          m_sidecarTable->setItem(i, 1, new QTableWidgetItem(m_sidecars[i].exePath));
      }
  }
  ```

  Then add a static helper at the top of AccountDialog.cpp (after the includes) for editing a single sidecar:

  ```cpp
  static bool editSidecar(AccountManager::SidecarProgram &sc, QWidget *parent)
  {
      QDialog dlg(parent);
      dlg.setWindowTitle(sc.id.isEmpty() ? "Add Sidecar" : "Edit Sidecar");
      dlg.setMinimumWidth(400);
      auto *layout = new QVBoxLayout(&dlg);
      auto *form = new QFormLayout;

      auto *nameEdit = new QLineEdit(sc.name);
      nameEdit->setPlaceholderText("e.g. Discord IPC Bridge");
      form->addRow("Name:", nameEdit);

      auto *pathEdit = new QLineEdit(sc.exePath);
      pathEdit->setPlaceholderText(R"(e.g. C:\tools\winediscordipcbridge.exe)");
      form->addRow("Exe path:", pathEdit);

      auto *argsEdit = new QLineEdit(sc.args.join(" "));
      argsEdit->setPlaceholderText("Optional arguments");
      form->addRow("Arguments:", argsEdit);

      layout->addLayout(form);
      auto *label = new QLabel("Use a Windows-style path relative to the Wine prefix.");
      label->setWordWrap(true);
      label->setStyleSheet("color: gray; font-size: 11px;");
      layout->addWidget(label);

      auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
      connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
      connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
      layout->addWidget(buttons);

      if (dlg.exec() != QDialog::Accepted) return false;
      if (nameEdit->text().trimmed().isEmpty() || pathEdit->text().trimmed().isEmpty())
          return false;

      sc.name = nameEdit->text().trimmed();
      sc.exePath = pathEdit->text().trimmed();
      sc.args = argsEdit->text().split(' ', Qt::SkipEmptyParts);
      return true;
  }
  ```

  Add required includes at the top of AccountDialog.cpp for the inline dialog:
  `#include <QFormLayout>` (already present), `#include <QLabel>` (add if missing).

  Now wire up the buttons inside `setupUi`, after the button/layout declarations:

  ```cpp
  connect(scAddBtn, &QPushButton::clicked, this, [this]() {
      AccountManager::SidecarProgram sc;
      sc.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
      if (editSidecar(sc, this)) {
          m_sidecars.append(sc);
          refreshSidecarTable();
      }
  });

  connect(scEditBtn, &QPushButton::clicked, this, [this]() {
      int row = m_sidecarTable->currentRow();
      if (row < 0 || row >= m_sidecars.size()) return;
      if (editSidecar(m_sidecars[row], this))
          refreshSidecarTable();
  });

  connect(scRemoveBtn, &QPushButton::clicked, this, [this]() {
      int row = m_sidecarTable->currentRow();
      if (row < 0 || row >= m_sidecars.size()) return;
      m_sidecars.removeAt(row);
      refreshSidecarTable();
  });

  connect(m_sidecarTable, &QTableWidget::doubleClicked, this, [this](const QModelIndex &idx) {
      int row = idx.row();
      if (row >= 0 && row < m_sidecars.size())
          if (editSidecar(m_sidecars[row], this))
              refreshSidecarTable();
  });
  ```

- [ ] **Step 4: Populate sidecars in setAccount**

  In `setAccount`, after `m_showWeeklyVaultCheck->setChecked(...)`, add:

  ```cpp
  m_sidecars = account.sidecars;
  refreshSidecarTable();
  ```

- [ ] **Step 5: Return sidecars in account() getter**

  Read the `account()` method in AccountDialog.cpp and find where it constructs the return value. Add after the last field assignment before `return`:

  ```cpp
  result.sidecars = m_sidecars;
  ```

- [ ] **Step 6: Build**

  ```bash
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && make -C build -j$(nproc) 2>&1 | tail -5
  ```

  Expected: build succeeds, no errors.

- [ ] **Step 7: Commit**

  ```bash
  git add src/ui/AccountDialog.h src/ui/AccountDialog.cpp
  git commit -m "Add Sidecar Programs list editor to AccountDialog"
  ```

---

### Task 3: Implement sidecar lifecycle in ProcessManager

**Files:**
- Modify: `src/core/ProcessManager.h`
- Modify: `src/core/ProcessManager.cpp`

- [ ] **Step 1: Add m_sidecars and private method declarations to ProcessManager.h**

  In the private section of `ProcessManager`, after the existing private member variables, add:

  ```cpp
  QMap<QString, QList<QProcess*>> m_sidecars;
  ```

  In the private methods section (near `writeUmuScript`, `buildEnvironment`, etc.), add:

  ```cpp
  void launchSidecars(const QString &accountId, const QString &winePrefix);
  void killSidecars(const QString &accountId);
  QString windowsToLinuxPath(const QString &winPath, const QString &winePrefix) const;
  ```

- [ ] **Step 2: Implement windowsToLinuxPath in ProcessManager.cpp**

  Add this function anywhere in ProcessManager.cpp (e.g. near the other small helpers like `uniqueAppId`):

  ```cpp
  QString ProcessManager::windowsToLinuxPath(const QString &winPath, const QString &winePrefix) const
  {
      QString path = winPath;
      path.replace('\\', '/');
      // Convert drive letter: C:/foo -> {winePrefix}/drive_c/foo
      if (path.size() >= 3 && path[1] == ':' && path[2] == '/') {
          QString drive = "drive_" + path[0].toLower();
          path = winePrefix + "/" + drive + "/" + path.mid(3);
      }
      return path;
  }
  ```

- [ ] **Step 3: Implement killSidecars in ProcessManager.cpp**

  Disconnect signals first so the `finished` lambda doesn't fire after we delete the process.

  ```cpp
  void ProcessManager::killSidecars(const QString &accountId)
  {
      if (!m_sidecars.contains(accountId)) return;
      for (auto *proc : m_sidecars[accountId]) {
          proc->disconnect();
          proc->terminate();
          if (!proc->waitForFinished(3000))
              proc->kill();
          delete proc;
      }
      m_sidecars.remove(accountId);
  }
  ```

- [ ] **Step 4: Implement launchSidecars in ProcessManager.cpp**

  Add `#include <QPointer>` to the includes at the top of ProcessManager.cpp if not already present.

  ```cpp
  void ProcessManager::launchSidecars(const QString &accountId, const QString &winePrefix)
  {
      auto acct = m_accounts->account(accountId);
      if (acct.sidecars.isEmpty()) return;

      QString umuBin = QStandardPaths::findExecutable("umu-run");
      if (umuBin.isEmpty()) umuBin = "umu-run";

      for (const auto &sidecar : acct.sidecars) {
          QString linuxPath = windowsToLinuxPath(sidecar.exePath, winePrefix);
          if (!QFile::exists(linuxPath)) {
              emit instanceOutput(accountId,
                  QString("Sidecar '%1': not found at %2, skipping.\n")
                      .arg(sidecar.name, linuxPath));
              continue;
          }

          QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
          env.insert("WINEPREFIX", winePrefix);
          if (!m_protonPath.isEmpty()) env.insert("PROTONPATH", m_protonPath);
          env.insert("GAMEID", "umu-1284210");
          env.insert("STORE", "none");
          for (auto it = acct.envVars.constBegin(); it != acct.envVars.constEnd(); ++it)
              env.insert(it.key(), it.value());

          auto *proc = new QProcess(this);
          proc->setProcessEnvironment(env);
          proc->setProcessChannelMode(QProcess::MergedChannels);

          QPointer<QProcess> procPtr = proc;
          connect(proc, &QProcess::readyReadStandardOutput, this, [this, procPtr, accountId]() {
              if (procPtr) emit instanceOutput(accountId, procPtr->readAllStandardOutput());
          });

          QString scName = sidecar.name;
          connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                  this, [this, accountId, procPtr, scName](int exitCode, QProcess::ExitStatus) {
              emit instanceOutput(accountId,
                  QString("Sidecar '%1' exited (code %2).\n").arg(scName).arg(exitCode));
              if (m_sidecars.contains(accountId) && procPtr)
                  m_sidecars[accountId].removeAll(procPtr.data());
              if (procPtr) procPtr->deleteLater();
          });

          QStringList args;
          args << linuxPath << sidecar.args;
          proc->start(umuBin, args);

          if (!proc->waitForStarted(5000)) {
              emit instanceOutput(accountId,
                  QString("Sidecar '%1': failed to start.\n").arg(scName));
              proc->deleteLater();
              continue;
          }

          m_sidecars[accountId].append(proc);
          emit instanceOutput(accountId,
              QString("Sidecar '%1' started (PID %2).\n")
                  .arg(scName).arg(proc->processId()));
      }
  }
  ```

- [ ] **Step 5: Call launchSidecars before GW2 starts — main account branch**

  In `launchAccount`, inside the `if (acct.isMain)` block, find the line `proc->start(...)` that launches the GW2 script. Just before it, add:

  ```cpp
  launchSidecars(accountId, basePrefix);
  ```

- [ ] **Step 6: Call launchSidecars before GW2 starts — alt account branch**

  In `launchAccount`, in the alt account path, find the line `proc->start(...)` that launches the GW2 script for the alt. Just before it, add:

  ```cpp
  launchSidecars(accountId, winePrefix);
  ```

  (`winePrefix` at that point holds the cloned prefix path.)

- [ ] **Step 7: Call killSidecars in stopAccount**

  In `stopAccount`, at the very start of the function body (before the state check), add:

  ```cpp
  killSidecars(accountId);
  ```

- [ ] **Step 8: Call killSidecars in the GW2 process finished handler**

  In `launchAccount`, the `QProcess::finished` lambda for the GW2 process logs output and does cleanup. Find that lambda and add `killSidecars(accountId);` as its first line, before any other cleanup. This handles the case where GW2 exits naturally (user closes it) without `stopAccount` being called first.

  There are two such lambdas — one in the `if (acct.isMain)` branch and one in the alt branch. Add the call to both.

- [ ] **Step 9: Build**

  ```bash
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && make -C build -j$(nproc) 2>&1 | tail -5
  ```

  Expected: build succeeds, no errors.

- [ ] **Step 10: Commit**

  ```bash
  git add src/core/ProcessManager.h src/core/ProcessManager.cpp
  git commit -m "Implement sidecar program launch and lifecycle in ProcessManager"
  ```

---

### Task 4: Manual smoke test

- [ ] **Step 1: Launch the app**

  ```bash
  ./build/sir-launchalot --dev
  ```

- [ ] **Step 2: Open AccountDialog for an account and verify the Sidecar Programs section appears**

  Right-click an account → Edit. Confirm the "Sidecar Programs" group box is visible with an empty table and Add/Edit/Remove buttons.

- [ ] **Step 3: Add a sidecar entry**

  Click Add. Enter name "Test", exe path `C:\windows\system32\notepad.exe`, no args. Click OK. Verify it appears in the table.

- [ ] **Step 4: Verify persistence**

  Click OK to save. Reopen the dialog. Verify the sidecar is still listed.

- [ ] **Step 5: Verify launch output**

  Launch the account. In the log panel, verify lines like:
  - `Sidecar 'Test' started (PID XXXXX).`
  - (or `Sidecar 'Test': not found at ..., skipping.` if the path doesn't exist in the prefix)

- [ ] **Step 6: Verify kill on stop**

  Stop the account. Verify the log shows `Sidecar 'Test' exited (code N).`

- [ ] **Step 7: Final commit (version bump optional)**

  ```bash
  git add -p
  git commit -m "Sidecar programs: smoke test passed"
  ```
