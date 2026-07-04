#pragma once

#include <QIcon>
#include <QColor>

// Coherent, purpose-drawn UI icons. Every glyph is rendered with the
// same QPainter code path (same canvas, same inset) so the toolbar
// reads as one consistent set - and it needs no image assets and no
// Qt SVG module (which TS3 does not ship).
//
// Each glyph has a logical signature color: play = green (go),
// pause = amber (hold), stop = red, reload = blue, sandbox = violet.
namespace IconFactory {

QIcon play(const QColor &c    = QColor(0x57, 0xC4, 0x5E));   // green
QIcon pause(const QColor &c   = QColor(0xE6, 0xA8, 0x3C));   // amber
QIcon stop(const QColor &c    = QColor(0xD9, 0x53, 0x4F));   // red
QIcon reload(const QColor &c  = QColor(0x52, 0x9F, 0xD9));   // blue
QIcon sandbox(const QColor &c = QColor(0xB1, 0x7F, 0xD4));   // violet
QIcon reverse(const QColor &c = QColor(0xE6, 0x9A, 0x3C));   // orange, left-pointing triangle + bar
QIcon clear(const QColor &c   = QColor(0xD9, 0x53, 0x4F));   // red filled circle + white X
// Vinyl record: dark disc + grooves + colored label. labelColor is
// the record label at the centre (default = the transport red family).
QIcon vinyl(const QColor &labelColor = QColor(0xC0, 0x39, 0x2B));

}
