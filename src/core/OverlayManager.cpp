#include "core/OverlayManager.h"

#include <QStandardPaths>

OverlayManager::OverlayManager(QObject *parent)
    : QObject(parent)
{
    m_dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                + "/accounts";
}

void OverlayManager::setDataDir(const QString &dataDir)
{
    m_dataDir = dataDir;
}

QString OverlayManager::dataDir() const
{
    return m_dataDir;
}
