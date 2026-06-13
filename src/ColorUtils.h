// src/ColorUtils.h
//----------------------------------
// RP Soundboard Source Code
// Centralised colour<->string (RRGGBBAA hex) conversion. Previously the
// implementation lived in SoundInfo.cpp while other modules re-parsed
// hex inline; this header is now the single source.
//----------------------------------

#pragma once

#include <QColor>
#include <QString>

inline QColor stringToColor(const QString &str)
{
    QRgb rgb = str.toUInt(nullptr, 16);
    // Don't construct directly from QRgb because alpha is ignored that way.
    return QColor(qRed(rgb), qGreen(rgb), qBlue(rgb), qAlpha(rgb));
}

inline QString colorToString(const QColor &col)
{
    return QString::number(col.rgba(), 16);
}
