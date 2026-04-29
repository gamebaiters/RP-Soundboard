#include "style_helper.h"
#include <QFile>
#include <QTextStream>
#include <QDebug>

QString StyleHelper::loadDarkStyle()
{
    QFile f(":/style/dark_style.qss");
    if (!f.exists()) {
        qWarning() << "Unable to load dark style sheet, file not found";
        return QString();
    }
    if (f.open(QFile::ReadOnly | QFile::Text)) {
        QTextStream ts(&f);
        return ts.readAll();
    }
    qWarning() << "Unable to open dark style sheet";
    return QString();
}
