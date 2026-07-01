#include "SandboxState.h"
#include <QJsonArray>
#include <algorithm>
#include <set>

const char *SandboxState::stageName(int stage)
{
    static const char *names[Stage_COUNT] = {
        "Paulstretch", "EQ", "Compressor", "Saturator", "Spatial",
        "Chorus", "Flanger", "Flangus", "Phaser", "Delay", "Reverb",
        "Limiter", "Bitcrush", "GenLoss",
        "Noise Gate", "De-esser", "Transient", "Dynamic EQ"
    };
    if (stage < 0 || stage >= Stage_COUNT) return "?";
    return names[stage];
}

void SandboxState::defaultPipelineOrder(int out[Stage_COUNT])
{
    for (int i = 0; i < Stage_COUNT; ++i) out[i] = i;
}

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
    o["spatialEngine"]    = spatialEngine;
    o["leiaReflEnable"]   = leiaReflEnable;
    o["leiaReflLevel"]    = static_cast<double>(leiaReflLevel);
    o["leiaRoomSize"]     = static_cast<double>(leiaRoomSize);
    o["leiaRoomType"]     = leiaRoomType;
    o["leiaClarity"]      = static_cast<double>(leiaClarity);
    o["leiaWidth"]        = static_cast<double>(leiaWidth);
    o["stretchEnabled"]   = stretchEnabled;
    o["stretchFactor"]    = stretchFactor;
    o["stretchWindowMs"]  = stretchWindowMs;
    o["eqEnabled"]        = eqEnabled;
    QJsonArray eq;
    for (int i = 0; i < 16; ++i) eq.append(eqBandDb[i]);
    o["eq"]               = eq;
    o["reverbWet"]        = reverbWet;

    o["compEnabled"]      = compEnabled;
    o["compThresholdDb"]  = static_cast<double>(compThresholdDb);
    o["compRatio"]        = static_cast<double>(compRatio);
    o["compAttackMs"]     = static_cast<double>(compAttackMs);
    o["compReleaseMs"]    = static_cast<double>(compReleaseMs);
    o["compKneeDb"]       = static_cast<double>(compKneeDb);
    o["compMakeupDb"]     = static_cast<double>(compMakeupDb);

    o["chorusEnabled"]    = chorusEnabled;
    o["chorusRate"]       = static_cast<double>(chorusRate);
    o["chorusDepth"]      = static_cast<double>(chorusDepth);
    o["chorusBaseDelay"]  = static_cast<double>(chorusBaseDelay);
    o["chorusVoices"]     = chorusVoices;
    o["chorusMix"]        = static_cast<double>(chorusMix);

    o["flangerEnabled"]   = flangerEnabled;
    o["flangerRate"]      = static_cast<double>(flangerRate);
    o["flangerDepth"]     = static_cast<double>(flangerDepth);
    o["flangerFeedback"]  = static_cast<double>(flangerFeedback);
    o["flangerBaseDelay"] = static_cast<double>(flangerBaseDelay);
    o["flangerMix"]       = static_cast<double>(flangerMix);

    o["flangusEnabled"]   = flangusEnabled;
    o["flangusRate"]      = static_cast<double>(flangusRate);
    o["flangusDepth"]     = static_cast<double>(flangusDepth);
    o["flangusFeedback"]  = static_cast<double>(flangusFeedback);
    o["flangusVoices"]    = flangusVoices;
    o["flangusSpread"]    = static_cast<double>(flangusSpread);
    o["flangusMix"]       = static_cast<double>(flangusMix);

    o["phaserEnabled"]    = phaserEnabled;
    o["phaserRate"]       = static_cast<double>(phaserRate);
    o["phaserDepth"]      = static_cast<double>(phaserDepth);
    o["phaserFeedback"]   = static_cast<double>(phaserFeedback);
    o["phaserStages"]     = phaserStages;
    o["phaserMix"]        = static_cast<double>(phaserMix);

    o["saturatorEnabled"] = saturatorEnabled;
    o["saturatorDrive"]   = static_cast<double>(saturatorDrive);
    o["saturatorMix"]     = static_cast<double>(saturatorMix);
    o["saturatorTone"]    = static_cast<double>(saturatorTone);
    o["saturatorMode"]    = saturatorMode;

    o["delayEnabled"]     = delayEnabled;
    o["delayTimeMs"]      = static_cast<double>(delayTimeMs);
    o["delayFeedback"]    = static_cast<double>(delayFeedback);
    o["delayMix"]         = static_cast<double>(delayMix);
    o["delayDamping"]     = static_cast<double>(delayDamping);
    o["delayPingPong"]    = delayPingPong;

    o["limiterEnabled"]   = limiterEnabled;
    o["limiterMode"]      = limiterMode;
    o["limiterCeiling"]   = static_cast<double>(limiterCeiling);
    o["limiterLookahead"] = static_cast<double>(limiterLookahead);
    o["limiterRelease"]   = static_cast<double>(limiterRelease);
    o["limiterRatio"]     = static_cast<double>(limiterRatio);
    o["limiterGateThresh"]= static_cast<double>(limiterGateThresh);

    o["bitcrusherEnabled"]  = bitcrusherEnabled;
    o["bitcrusherBitDepth"] = bitcrusherBitDepth;
    o["bitcrusherRate"]     = static_cast<double>(bitcrusherRate);

    o["monoEnabled"]        = monoEnabled;

    o["genLossEnabled"]     = genLossEnabled;
    o["genLossGenerations"] = genLossGenerations;
    o["randomEnabled"]      = randomEnabled;
    o["randomPitchCents"]   = randomPitchCents;
    o["duckSource"]         = duckSource;
    o["duckOthersDb"]       = static_cast<double>(duckOthersDb);

    o["gateEnabled"]        = gateEnabled;
    o["gateThresholdDb"]    = static_cast<double>(gateThresholdDb);
    o["gateRangeDb"]        = static_cast<double>(gateRangeDb);
    o["gateAttackMs"]       = static_cast<double>(gateAttackMs);
    o["gateHoldMs"]         = static_cast<double>(gateHoldMs);
    o["gateReleaseMs"]      = static_cast<double>(gateReleaseMs);
    o["gateSidechainSlot"]  = gateSidechainSlot;

    o["deesserEnabled"]      = deesserEnabled;
    o["deesserFreqHz"]       = static_cast<double>(deesserFreqHz);
    o["deesserQ"]            = static_cast<double>(deesserQ);
    o["deesserThresholdDb"]  = static_cast<double>(deesserThresholdDb);
    o["deesserRangeDb"]      = static_cast<double>(deesserRangeDb);
    o["deesserAttackMs"]     = static_cast<double>(deesserAttackMs);
    o["deesserReleaseMs"]    = static_cast<double>(deesserReleaseMs);
    o["deesserSidechainSlot"]= deesserSidechainSlot;

    o["transEnabled"]   = transEnabled;
    o["transAttackDb"]  = static_cast<double>(transAttackDb);
    o["transSustainDb"] = static_cast<double>(transSustainDb);

    o["dyneqEnabled"] = dyneqEnabled;
    QJsonArray bands;
    for (int i = 0; i < 4; ++i) {
        QJsonObject b;
        b["enabled"]      = dyneqBands[i].enabled;
        b["freq"]         = static_cast<double>(dyneqBands[i].freq);
        b["q"]            = static_cast<double>(dyneqBands[i].q);
        b["staticGainDb"] = static_cast<double>(dyneqBands[i].staticGainDb);
        b["thresholdDb"]  = static_cast<double>(dyneqBands[i].thresholdDb);
        b["ratio"]        = static_cast<double>(dyneqBands[i].ratio);
        b["dynamicDb"]    = static_cast<double>(dyneqBands[i].dynamicDb);
        b["attackMs"]     = static_cast<double>(dyneqBands[i].attackMs);
        b["releaseMs"]    = static_cast<double>(dyneqBands[i].releaseMs);
        bands.append(b);
    }
    o["dyneqBands"] = bands;

    o["compSidechainSlot"] = compSidechainSlot;

    o["dopplerEnabled"]  = dopplerEnabled;
    o["dopplerStrength"] = static_cast<double>(dopplerStrength);

    // Key is versioned: the DspStage enum was renumbered (Mono dropped,
    // Paulstretch added). Old "pipelineOrder" arrays carry stale indices
    // - using a new key makes pre-existing INIs/presets fall back to the
    // new default order instead of silently scrambling.
    QJsonArray pipe;
    for (int i = 0; i < Stage_COUNT; ++i) pipe.append(pipelineOrder[i]);
    o["pipelineOrderV2"] = pipe;

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
    s.spatialEngine   = o.value("spatialEngine").toInt(Engine_Leia);
    s.leiaReflEnable  = o.value("leiaReflEnable").toBool(true);
    s.leiaReflLevel   = static_cast<float>(o.value("leiaReflLevel").toDouble(-6.0));
    s.leiaRoomSize    = static_cast<float>(o.value("leiaRoomSize").toDouble(12.0));
    s.leiaRoomType    = o.value("leiaRoomType").toInt(1);
    s.leiaClarity     = static_cast<float>(o.value("leiaClarity").toDouble(100.0));
    s.leiaWidth       = static_cast<float>(o.value("leiaWidth").toDouble(35.0));
    s.stretchEnabled  = o.value("stretchEnabled").toBool(false);
    s.stretchFactor   = static_cast<float>(o.value("stretchFactor").toDouble(4.0));
    s.stretchWindowMs = static_cast<float>(o.value("stretchWindowMs").toDouble(180.0));
    s.eqEnabled       = o.value("eqEnabled").toBool(true);
    QJsonArray eq = o.value("eq").toArray();
    for (int i = 0; i < 16 && i < eq.size(); ++i)
        s.eqBandDb[i] = static_cast<float>(eq[i].toDouble(0.0));
    s.reverbWet       = static_cast<float>(o.value("reverbWet").toDouble(0.0));

    s.compEnabled      = o.value("compEnabled").toBool(false);
    s.compThresholdDb  = static_cast<float>(o.value("compThresholdDb").toDouble(-20.0));
    s.compRatio        = static_cast<float>(o.value("compRatio").toDouble(4.0));
    s.compAttackMs     = static_cast<float>(o.value("compAttackMs").toDouble(10.0));
    s.compReleaseMs    = static_cast<float>(o.value("compReleaseMs").toDouble(100.0));
    s.compKneeDb       = static_cast<float>(o.value("compKneeDb").toDouble(6.0));
    s.compMakeupDb     = static_cast<float>(o.value("compMakeupDb").toDouble(0.0));

    s.chorusEnabled    = o.value("chorusEnabled").toBool(false);
    s.chorusRate       = static_cast<float>(o.value("chorusRate").toDouble(1.0));
    s.chorusDepth      = static_cast<float>(o.value("chorusDepth").toDouble(3.0));
    s.chorusBaseDelay  = static_cast<float>(o.value("chorusBaseDelay").toDouble(10.0));
    s.chorusVoices     = o.value("chorusVoices").toInt(2);
    s.chorusMix        = static_cast<float>(o.value("chorusMix").toDouble(0.0));

    s.flangerEnabled   = o.value("flangerEnabled").toBool(false);
    s.flangerRate      = static_cast<float>(o.value("flangerRate").toDouble(0.5));
    s.flangerDepth     = static_cast<float>(o.value("flangerDepth").toDouble(0.7));
    s.flangerFeedback  = static_cast<float>(o.value("flangerFeedback").toDouble(0.5));
    s.flangerBaseDelay = static_cast<float>(o.value("flangerBaseDelay").toDouble(2.0));
    s.flangerMix       = static_cast<float>(o.value("flangerMix").toDouble(0.0));

    s.flangusEnabled   = o.value("flangusEnabled").toBool(false);
    s.flangusRate      = static_cast<float>(o.value("flangusRate").toDouble(0.8));
    s.flangusDepth     = static_cast<float>(o.value("flangusDepth").toDouble(0.5));
    s.flangusFeedback  = static_cast<float>(o.value("flangusFeedback").toDouble(0.3));
    s.flangusVoices    = o.value("flangusVoices").toInt(3);
    s.flangusSpread    = static_cast<float>(o.value("flangusSpread").toDouble(0.5));
    s.flangusMix       = static_cast<float>(o.value("flangusMix").toDouble(0.0));

    s.phaserEnabled    = o.value("phaserEnabled").toBool(false);
    s.phaserRate       = static_cast<float>(o.value("phaserRate").toDouble(0.5));
    s.phaserDepth      = static_cast<float>(o.value("phaserDepth").toDouble(0.7));
    s.phaserFeedback   = static_cast<float>(o.value("phaserFeedback").toDouble(0.3));
    s.phaserStages     = o.value("phaserStages").toInt(6);
    s.phaserMix        = static_cast<float>(o.value("phaserMix").toDouble(0.0));

    s.saturatorEnabled = o.value("saturatorEnabled").toBool(false);
    s.saturatorDrive   = static_cast<float>(o.value("saturatorDrive").toDouble(2.0));
    s.saturatorMix     = static_cast<float>(o.value("saturatorMix").toDouble(0.0));
    s.saturatorTone    = static_cast<float>(o.value("saturatorTone").toDouble(8000.0));
    s.saturatorMode    = o.value("saturatorMode").toInt(0);

    s.delayEnabled     = o.value("delayEnabled").toBool(false);
    s.delayTimeMs      = static_cast<float>(o.value("delayTimeMs").toDouble(300.0));
    s.delayFeedback    = static_cast<float>(o.value("delayFeedback").toDouble(0.4));
    s.delayMix         = static_cast<float>(o.value("delayMix").toDouble(0.0));
    s.delayDamping     = static_cast<float>(o.value("delayDamping").toDouble(5000.0));
    s.delayPingPong    = o.value("delayPingPong").toBool(false);

    s.limiterEnabled   = o.value("limiterEnabled").toBool(false);
    s.limiterMode      = o.value("limiterMode").toInt(0);
    s.limiterCeiling   = static_cast<float>(o.value("limiterCeiling").toDouble(-0.3));
    s.limiterLookahead = static_cast<float>(o.value("limiterLookahead").toDouble(1.0));
    s.limiterRelease   = static_cast<float>(o.value("limiterRelease").toDouble(100.0));
    s.limiterRatio     = static_cast<float>(o.value("limiterRatio").toDouble(4.0));
    s.limiterGateThresh= static_cast<float>(o.value("limiterGateThresh").toDouble(-60.0));

    s.bitcrusherEnabled  = o.value("bitcrusherEnabled").toBool(false);
    s.bitcrusherBitDepth = o.value("bitcrusherBitDepth").toInt(16);
    s.bitcrusherRate     = static_cast<float>(o.value("bitcrusherRate").toDouble(48000.0));

    s.monoEnabled        = o.value("monoEnabled").toBool(false);

    s.genLossEnabled     = o.value("genLossEnabled").toBool(false);
    s.genLossGenerations = o.value("genLossGenerations").toInt(1);
    s.randomEnabled      = o.value("randomEnabled").toBool(false);
    s.randomPitchCents   = o.value("randomPitchCents").toInt(0);
    s.duckSource         = o.value("duckSource").toBool(false);
    s.duckOthersDb       = static_cast<float>(o.value("duckOthersDb").toDouble(-12.0));

    s.gateEnabled        = o.value("gateEnabled").toBool(false);
    s.gateThresholdDb    = static_cast<float>(o.value("gateThresholdDb").toDouble(-40.0));
    s.gateRangeDb        = static_cast<float>(o.value("gateRangeDb").toDouble(-60.0));
    s.gateAttackMs       = static_cast<float>(o.value("gateAttackMs").toDouble(2.0));
    s.gateHoldMs         = static_cast<float>(o.value("gateHoldMs").toDouble(20.0));
    s.gateReleaseMs      = static_cast<float>(o.value("gateReleaseMs").toDouble(150.0));
    s.gateSidechainSlot  = o.value("gateSidechainSlot").toInt(-1);

    s.deesserEnabled     = o.value("deesserEnabled").toBool(false);
    s.deesserFreqHz      = static_cast<float>(o.value("deesserFreqHz").toDouble(6500.0));
    s.deesserQ           = static_cast<float>(o.value("deesserQ").toDouble(3.0));
    s.deesserThresholdDb = static_cast<float>(o.value("deesserThresholdDb").toDouble(-30.0));
    s.deesserRangeDb     = static_cast<float>(o.value("deesserRangeDb").toDouble(-10.0));
    s.deesserAttackMs    = static_cast<float>(o.value("deesserAttackMs").toDouble(3.0));
    s.deesserReleaseMs   = static_cast<float>(o.value("deesserReleaseMs").toDouble(80.0));
    s.deesserSidechainSlot = o.value("deesserSidechainSlot").toInt(-1);

    s.transEnabled       = o.value("transEnabled").toBool(false);
    s.transAttackDb      = static_cast<float>(o.value("transAttackDb").toDouble(0.0));
    s.transSustainDb     = static_cast<float>(o.value("transSustainDb").toDouble(0.0));

    s.dyneqEnabled       = o.value("dyneqEnabled").toBool(false);
    QJsonArray bands = o.value("dyneqBands").toArray();
    for (int i = 0; i < 4 && i < bands.size(); ++i) {
        QJsonObject b = bands[i].toObject();
        s.dyneqBands[i].enabled      = b.value("enabled").toBool(false);
        s.dyneqBands[i].freq         = static_cast<float>(b.value("freq").toDouble(s.dyneqBands[i].freq));
        s.dyneqBands[i].q            = static_cast<float>(b.value("q").toDouble(s.dyneqBands[i].q));
        s.dyneqBands[i].staticGainDb = static_cast<float>(b.value("staticGainDb").toDouble(0.0));
        s.dyneqBands[i].thresholdDb  = static_cast<float>(b.value("thresholdDb").toDouble(-30.0));
        s.dyneqBands[i].ratio        = static_cast<float>(b.value("ratio").toDouble(2.0));
        s.dyneqBands[i].dynamicDb    = static_cast<float>(b.value("dynamicDb").toDouble(-6.0));
        s.dyneqBands[i].attackMs     = static_cast<float>(b.value("attackMs").toDouble(15.0));
        s.dyneqBands[i].releaseMs    = static_cast<float>(b.value("releaseMs").toDouble(150.0));
    }

    s.compSidechainSlot  = o.value("compSidechainSlot").toInt(-1);

    s.dopplerEnabled     = o.value("dopplerEnabled").toBool(false);
    s.dopplerStrength    = static_cast<float>(o.value("dopplerStrength").toDouble(50.0));

    QJsonArray pipe = o.value("pipelineOrderV2").toArray();
    if (pipe.size() >= 1 && pipe.size() <= Stage_COUNT) {
        std::set<int> seen;
        bool valid = true;
        int count = pipe.size();
        for (int i = 0; i < count; ++i) {
            int v = pipe[i].toInt(-1);
            if (v < 0 || v >= Stage_COUNT || !seen.insert(v).second) { valid = false; break; }
            s.pipelineOrder[i] = v;
        }
        if (valid && count < Stage_COUNT) {
            for (int st = 0; st < Stage_COUNT; ++st) {
                if (seen.find(st) == seen.end())
                    s.pipelineOrder[count++] = st;
            }
        }
        if (!valid) defaultPipelineOrder(s.pipelineOrder);
    }

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
    if (spatialEngine != d.spatialEngine) return true;
    if (stretchEnabled != d.stretchEnabled) return true;
    if (reverbWet != d.reverbWet) return true;
    for (int i = 0; i < 16; ++i) if (eqBandDb[i] != 0.0f) return true;
    if (compEnabled) return true;
    if (chorusEnabled) return true;
    if (flangerEnabled) return true;
    if (flangusEnabled) return true;
    if (phaserEnabled) return true;
    if (saturatorEnabled) return true;
    if (delayEnabled) return true;
    if (limiterEnabled) return true;
    if (bitcrusherEnabled) return true;
    if (monoEnabled) return true;
    if (genLossEnabled) return true;
    return false;
}
