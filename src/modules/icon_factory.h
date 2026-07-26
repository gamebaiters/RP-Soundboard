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

// Line-art action set (matches the mic-channel preset glyphs: light
// stroke, no fill) used to replace text buttons across the channel
// row and the sandbox dialog.
QIcon save(const QColor &c      = QColor(0xDC, 0xDC, 0xDC));  // floppy disk
QIcon trash(const QColor &c     = QColor(0xDC, 0xDC, 0xDC));  // delete
QIcon copyDoc(const QColor &c   = QColor(0xDC, 0xDC, 0xDC));  // two sheets
QIcon paste(const QColor &c     = QColor(0xDC, 0xDC, 0xDC));  // clipboard
QIcon folderOpen(const QColor &c= QColor(0xDC, 0xDC, 0xDC));  // load
QIcon download(const QColor &c  = QColor(0xFF, 0xFF, 0xFF));  // arrow into tray
QIcon exportAudio(const QColor &c = QColor(0xDC, 0xDC, 0xDC));// arrow out of tray + wave
QIcon record(const QColor &c    = QColor(0xD9, 0x53, 0x4F));  // red dot
// Checklist: three rows, each a small box + a rule. Used for the
// "which DSP modules are visible" popup next to the pipeline reset.
// Painted rather than a ☰ glyph — the TS3 client font renders that as
// tofu (same reason the playlist button has a hand-drawn icon).
QIcon checklist(const QColor &c = QColor(0xDC, 0xDC, 0xDC));

// Soundboard identity glyph: a 2x2 pad grid (launchpad style) with
// colored pads - used for the button injected into the TS3 client
// toolbar, where it must read as "soundboard" at 20-24 px.
QIcon soundboard();

}
