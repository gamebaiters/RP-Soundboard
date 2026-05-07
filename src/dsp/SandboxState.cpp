#include "SandboxState.h"
#include <QJsonArray>

QJsonObject SandboxState::toJson() const
{
    QJsonObject o;
    o["enabled"]          = enabled;
    o["spatialMode"]      = spatialMode;
    o["panValue"]         = panValue;
    o["posX"]             = posX;
    o["posY"]             = posY;
    o["elev"]             = elev;
    o["distanceM"]        = distanceM;
    o["stereoWidthDeg"]   = stereoWidthDeg;
    o["headSway"]         = headSway;
    o["spatialMix"]       = spatialMix;
    o["rotateRpm"]        = rotateRpm;
    o["rotateRadiusM"]    = rotateRadiusM;
    o["rotateCcw"]        = rotateCcw;
    o["rotateElev"]       = rotateElev;
    o["stretchEnabled"]   = stretchEnabled;
    o["stretchFactor"]    = stretchFactor;
    o["stretchWindowMs"]  = stretchWindowMs;
    o["eqEnabled"]        = eqEnabled;
    QJsonArray eq;
    for (int i = 0; i < 16; ++i) eq.append(eqBandDb[i]);
    o["eq"]               = eq;
    o["reverbWet"]        = reverbWet;
    return o;
}

SandboxState SandboxState::fromJson(const QJsonObject &o)
{
    SandboxState s;
    s.enabled         = o.value("enabled").toBool(false);
    s.spatialMode     = o.value("spatialMode").toInt(Spatial_Off);
    s.panValue        = static_cast<float>(o.value("panValue").toDouble(0.0));
    s.posX            = static_cast<float>(o.value("posX").toDouble(0.0));
    s.posY            = static_cast<float>(o.value("posY").toDouble(0.0));
    s.elev            = static_cast<float>(o.value("elev").toDouble(0.0));
    s.distanceM       = static_cast<float>(o.value("distanceM").toDouble(1.5));
    s.stereoWidthDeg  = static_cast<float>(o.value("stereoWidthDeg").toDouble(60.0));
    s.headSway        = o.value("headSway").toBool(false);
    s.spatialMix      = static_cast<float>(o.value("spatialMix").toDouble(1.0));
    s.rotateRpm       = static_cast<float>(o.value("rotateRpm").toDouble(15.0));
    s.rotateRadiusM   = static_cast<float>(o.value("rotateRadiusM").toDouble(1.5));
    s.rotateCcw       = o.value("rotateCcw").toBool(false);
    s.rotateElev      = static_cast<float>(o.value("rotateElev").toDouble(0.0));
    s.stretchEnabled  = o.value("stretchEnabled").toBool(false);
    s.stretchFactor   = static_cast<float>(o.value("stretchFactor").toDouble(4.0));
    s.stretchWindowMs = static_cast<float>(o.value("stretchWindowMs").toDouble(180.0));
    s.eqEnabled       = o.value("eqEnabled").toBool(true);
    QJsonArray eq = o.value("eq").toArray();
    for (int i = 0; i < 16 && i < eq.size(); ++i)
        s.eqBandDb[i] = static_cast<float>(eq[i].toDouble(0.0));
    s.reverbWet       = static_cast<float>(o.value("reverbWet").toDouble(0.0));
    return s;
}

void SandboxState::resetToDefaults()
{
    *this = SandboxState();
}

bool SandboxState::isModified() const
{
    SandboxState d;
    if (enabled != d.enabled) return true;
    if (spatialMode != d.spatialMode) return true;
    if (stretchEnabled != d.stretchEnabled) return true;
    if (reverbWet != d.reverbWet) return true;
    for (int i = 0; i < 16; ++i) if (eqBandDb[i] != 0.0f) return true;
    return false;
}
