// src/howto_qt.cpp - GameBaiters Soundboard How-To guide
//----------------------------------
// GameBaiters Soundboard
// Copyright (c) 2026 GameBaitersCrew
//----------------------------------

#include "howto_qt.h"
#include "modules/theme.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTextBrowser>
#include <QPushButton>
#include <QLabel>
#include <QEvent>
#include <QVector>

namespace {
struct Section { QString anchor; QString title; QString body; };
}

HowToDialog::HowToDialog(QWidget *parent)
    : QWidget(parent, Qt::Window | Qt::WindowTitleHint |
                      Qt::WindowCloseButtonHint | Qt::WindowMaximizeButtonHint)
{
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(Theme::compositeStyleSheet());
    Theme::trackThemedWidget(this);
    resize(720, 640);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 14, 16, 12);
    root->setSpacing(8);

    auto *header = new QLabel(this);
    header->setObjectName("howToHeader");
    header->setStyleSheet("font-size: 16px; font-weight: bold;");
    header->setText(tr("How to use the GameBaiters Soundboard"));
    root->addWidget(header);

    m_browser = new QTextBrowser(this);
    m_browser->setOpenExternalLinks(true);
    root->addWidget(m_browser, 1);

    auto *btnRow = new QHBoxLayout;
    btnRow->addStretch(1);
    auto *closeBtn = new QPushButton(tr("Close"), this);
    connect(closeBtn, &QPushButton::clicked, this, &QWidget::close);
    btnRow->addWidget(closeBtn);
    root->addLayout(btnRow);

    rebuildContent();
}

void HowToDialog::changeEvent(QEvent *e)
{
    // Live language switch: re-translate the whole guide in place.
    if (e->type() == QEvent::LanguageChange) {
        setWindowTitle(tr("How to use the soundboard"));
        if (auto *h = findChild<QLabel*>("howToHeader"))
            h->setText(tr("How to use the GameBaiters Soundboard"));
        rebuildContent();
    }
    QWidget::changeEvent(e);
}

void HowToDialog::rebuildContent()
{
    setWindowTitle(tr("How to use the soundboard"));

    QVector<Section> s;
    s.append({ "start", tr("1. Getting started"), tr(
        "<p>The soundboard runs as a plugin inside TeamSpeak 3. Open it from "
        "the <b>Plugins</b> menu &rarr; <b>Open GameBaiters Soundboard</b>.</p>"
        "<p>You must be <b>connected to a server</b> for sound to be sent. While "
        "disconnected the window is dimmed and shows a notice &mdash; the "
        "interface still lets you arrange buttons and tweak effects.</p>"
        "<p>The window has three areas: the <b>sound button grid</b> on top, "
        "the <b>channels</b> in the middle, and the <b>control bar</b> at the "
        "bottom (add channel, stop/pause all, profiles, settings).</p>") });

    s.append({ "buttons", tr("2. Sound buttons"), tr(
        "<p>Each cell in the grid is a button. To assign a sound, <b>drag an "
        "audio file</b> onto a cell, or right-click a cell to open its editor.</p>"
        "<p>Supported formats: MP3, WAV, OGG, FLAC, AAC, WMA, MP4, MKV, AVI, FLV "
        "and anything else FFmpeg can decode.</p>"
        "<p>There are three button types:</p>"
        "<ul>"
        "<li><b>Audio</b> &mdash; plays a sound file.</li>"
        "<li><b>Macro</b> &mdash; restores a saved snapshot of every channel "
        "(see section 12).</li>"
        "<li><b>Special</b> &mdash; bound actions such as stop-all.</li>"
        "</ul>"
        "<p>In a button's editor you can set a <b>custom label</b>, "
        "<b>background colour</b>, <b>background image</b>, a <b>hotkey</b>, "
        "looping, and a start/stop <b>crop</b> range. Drag buttons to "
        "rearrange them. Use the <b>search bar</b> above the grid to filter "
        "buttons by label. Grid size (rows &times; columns) is set in Settings.</p>") });

    s.append({ "play", tr("3. Playing and transport"), tr(
        "<p>Click a button to play its sound. The waveform view shows playback "
        "progress &mdash; <b>click the waveform</b> to seek.</p>"
        "<p>Transport controls: <b>Play</b>, <b>Pause</b>, <b>Stop</b>, and skip "
        "&plusmn;5 s / &plusmn;10 s. <b>Stop all</b> and <b>Pause all</b> in the "
        "bottom bar affect every channel at once.</p>"
        "<p>Each button has a <b>Loop</b> toggle and an independent "
        "<b>per-button volume</b> modifier, separate from the channel volume.</p>") });

    s.append({ "channels", tr("4. Channels"), tr(
        "<p>A <b>channel</b> is one independent playback lane: it has its own "
        "file, volume, FX, waveform, transport and audio sandbox. Add channels "
        "with <b>+ Add channel</b>; the first channel is always present.</p>"
        "<p>Channel titles are editable &mdash; click the title to rename it. "
        "The <b>peak meter</b> shows level (cyan = normal, red = clipping). "
        "Each channel's state is saved and restored across restarts.</p>") });

    s.append({ "volume", tr("5. Volume control"), tr(
        "<p>Every channel has <b>two</b> volume sliders:</p>"
        "<ul>"
        "<li><b>Local</b> &mdash; how loud <i>you</i> hear the sound.</li>"
        "<li><b>Remote</b> &mdash; how loud <i>others</i> on the server hear it.</li>"
        "</ul>"
        "<p>The <b>Link</b> toggle moves both sliders together. A <b>Global "
        "volume</b> master scales everything. Turn on <b>Earrape protection</b> "
        "in Settings to apply a safety limiter on what you hear.</p>") });

    s.append({ "fx", tr("6. FX panel (pitch / speed / reverb)"), tr(
        "<p>The compact <b>FX panel</b> on each channel offers three quick, "
        "real-time effects:</p>"
        "<ul>"
        "<li><b>Pitch</b> &mdash; shift pitch without changing speed "
        "(0.33&times;&ndash;3.0&times;).</li>"
        "<li><b>Speed</b> &mdash; change playback rate without changing pitch.</li>"
        "<li><b>Reverb</b> &mdash; a transient room ambience (0&ndash;100).</li>"
        "</ul>"
        "<p>The <b>Sync</b> toggle locks pitch and speed together for a "
        "chipmunk / slow-motion effect. With <b>Remember pitch/speed/reverb</b> "
        "enabled in Settings, these values persist across restarts.</p>"
        "<p>The FX panel is the <i>quick</i> effects strip. For the deep "
        "14-effect chain, use the <b>Audio sandbox</b> below.</p>") });

    s.append({ "sandbox", tr("7. The audio sandbox"), tr(
        "<p>The <b>audio sandbox</b> is a per-channel rack of 14 DSP effects. "
        "Open it from the channel's sandbox button.</p>"
        "<p>A single <b>master switch</b> at the top enables or bypasses the "
        "whole chain at zero CPU cost &mdash; every value is kept so you can "
        "A/B instantly.</p>"
        "<p>The <b>Pipeline order</b> bar shows the processing chain as colored "
        "blocks. <b>Drag</b> a block to reorder the chain; <b>click</b> a block "
        "(without dragging) to jump straight to that effect's panel and open "
        "it. The panel list below mirrors the pipeline order. <b>Reset order</b> "
        "restores the default chain (effect values are kept).</p>"
        "<p>Use the <b>search box</b> to filter the module list. Each module "
        "has its own enable toggle and a small reset button. The "
        "<b>Fold output to mono</b> checkbox collapses the stereo image and is "
        "always applied last &mdash; it is not a reorderable module.</p>"
        "<p>The whole sandbox state is saved per channel and travels inside "
        "sandbox presets, including the pipeline order.</p>") });

    s.append({ "spatial", tr("8. Spatial audio / HRTF"), tr(
        "<p>The <b>Spatial</b> stage places the sound in 3D space. Modes:</p>"
        "<ul>"
        "<li><b>Off</b> &mdash; no spatial processing.</li>"
        "<li><b>L/R Pan</b> &mdash; simple equal-power stereo balance.</li>"
        "<li><b>3D Manual</b> &mdash; drag a virtual source on the positional "
        "pad; set elevation, distance and stereo width.</li>"
        "<li><b>3D Rotate</b> &mdash; the source orbits your head at a chosen "
        "RPM and radius.</li>"
        "<li><b>8D preset</b> &mdash; the classic 'YouTube 8D' recipe: orbit + "
        "head sway + tight width.</li>"
        "</ul>"
        "<p><b>Spatial mix</b> crossfades between processed and raw stereo "
        "&mdash; pull it back if the result feels too narrow. <b>Ambience</b> "
        "adds room reflections that push the sound out of your head.</p>") });

    s.append({ "stretch", tr("9. Paulstretch"), tr(
        "<p><b>Paulstretch</b> is an extreme time-stretch (1&times;&ndash;50&times;) "
        "that keeps pitch while slowing the sound into a wide, diffuse drone. "
        "It is the first module in the sandbox list.</p>"
        "<p><b>Factor</b> sets how much slower it plays; <b>Window</b> sets the "
        "FFT analysis size (bigger = smoother and more 'frozen', smaller = "
        "grittier). Its pipeline position is fixed &mdash; it runs on a "
        "separate streaming feed before the rest of the chain.</p>"
        "<p>Paulstretch is a <b>local</b> effect: while it is on, the server "
        "receives a clean stream, like preview-only mode.</p>") });

    s.append({ "effects", tr("10. The DSP effects"), tr(
        "<ul>"
        "<li><b>16-band EQ</b> &mdash; ISO 2/3-octave graphic equaliser, "
        "&plusmn;12 dB per band.</li>"
        "<li><b>Compressor</b> &mdash; evens out dynamics (threshold, ratio, "
        "attack, release, knee, makeup).</li>"
        "<li><b>Saturator</b> &mdash; adds warmth/grit; soft, tube, tape or "
        "hard-clip modes.</li>"
        "<li><b>Chorus</b> &mdash; thickens the sound with detuned voices.</li>"
        "<li><b>Flanger</b> &mdash; classic sweeping jet effect.</li>"
        "<li><b>Flangus</b> &mdash; flanger/chorus hybrid with multi-voice "
        "spread.</li>"
        "<li><b>Phaser</b> &mdash; sweeping notch filter, up to 12 stages.</li>"
        "<li><b>Delay</b> &mdash; echo up to 3 s, with damping and optional "
        "ping-pong.</li>"
        "<li><b>Reverb</b> &mdash; room ambience (the Ambience slider).</li>"
        "<li><b>Limiter</b> &mdash; brick-wall limiter, compressor or noise "
        "gate.</li>"
        "<li><b>Bitcrusher</b> &mdash; reduces bit depth and sample rate for a "
        "lo-fi sound.</li>"
        "<li><b>Generation Loss</b> &mdash; simulates repeated re-encoding "
        "(the 'deep-fried' effect).</li>"
        "</ul>"
        "<p>Every effect has an enable toggle and persists across restarts.</p>") });

    s.append({ "presets", tr("11. Presets"), tr(
        "<p>You can save and reload named presets:</p>"
        "<ul>"
        "<li><b>EQ presets</b> &mdash; just the 16 EQ bands.</li>"
        "<li><b>Sandbox presets</b> &mdash; every effect, parameter and the "
        "pipeline order.</li>"
        "</ul>"
        "<p><b>Copy / Paste</b> buttons move a setup between channels through "
        "the clipboard as a short text string.</p>") });

    s.append({ "macros", tr("12. Macros"), tr(
        "<p>A <b>macro button</b> freezes the state of <i>every</i> channel "
        "(volumes, FX, files, full sandbox state) into one button. Pressing it "
        "later restores that whole snapshot at once.</p>"
        "<p><b>Restore pre-macro</b> reverts to the state the channels were in "
        "just before the macro fired. Macro buttons carry a visual marker.</p>") });

    s.append({ "profiles", tr("13. Profiles"), tr(
        "<p>There are <b>4 independent profiles</b>, each with its own button "
        "grid, grid size and macros. Switch profiles with the P1&ndash;P4 "
        "buttons or a hotkey. Profiles can be exported and imported as JSON "
        "files to back up or share a setup.</p>") });

    s.append({ "hotkeys", tr("14. Hotkeys"), tr(
        "<p>Any button can be bound to a <b>hotkey</b> through the TeamSpeak "
        "hotkey system (set it in the button editor). Enable <b>Show hotkeys "
        "on buttons</b> in Settings to display the binding on each button.</p>"
        "<p><b>Disable hotkeys</b> in Settings temporarily ignores all "
        "soundboard hotkeys without losing the bindings.</p>") });

    s.append({ "export", tr("15. Audio export"), tr(
        "<p><b>Audio export</b> renders a sound to a 48 kHz stereo WAV file "
        "with the <i>entire</i> signal chain baked in: pitch, speed, reverb, "
        "all sandbox DSP and Paulstretch. Export runs in the background with a "
        "progress dialog and can be cancelled.</p>") });

    s.append({ "theme", tr("16. Theme"), tr(
        "<p>In Settings you can recolour the soundboard with three colours "
        "(accent, waveform, background) plus a contrast slider. Over a dozen "
        "surface colours are derived automatically and applied live. The theme "
        "is scoped to the soundboard and never leaks into the TeamSpeak "
        "client.</p>") });

    s.append({ "settings", tr("17. Settings"), tr(
        "<p>The Settings window collects every option: grid size, language, "
        "earrape protection, link volumes, remember FX, restore session on "
        "startup, hide waveform, audio meter visibility, hotkey display, the "
        "theme colours, and the reset buttons. Use the reset buttons there to "
        "restore channels (which also resets their DSP pipeline order).</p>"
        "<p><b>Language</b>: the interface auto-selects Italian when the "
        "system locale is Italian, English otherwise. You can override it "
        "here at any time &mdash; the change applies instantly.</p>") });

    s.append({ "updates", tr("18. Updates"), tr(
        "<p>The plugin checks for updates on startup. Run a manual check from "
        "the <b>Plugins</b> menu &rarr; <b>Check for update</b>. When a newer "
        "version exists it is downloaded from GitHub Releases for your "
        "platform.</p>") });

    // ---- assemble HTML ----
    QString toc;
    QString bodyHtml;
    for (const Section &sec : s) {
        toc += QString("<li><a href=\"#%1\">%2</a></li>")
                   .arg(sec.anchor, sec.title.toHtmlEscaped());
        bodyHtml += QString("<h2><a name=\"%1\"></a>%2</h2>%3<p>&nbsp;</p>")
                        .arg(sec.anchor, sec.title.toHtmlEscaped(), sec.body);
    }

    // Pull live theme colors so the guide is readable under any theme.
    Theme::Derived d = Theme::derive(Theme::colors());
    const QString bg     = d.bg.name();
    const QString text   = d.text.name();
    const QString muted  = d.textMuted.name();
    const QString accent = d.accentLight.isValid() ? d.accentLight.name()
                                                   : d.accent.name();
    const QString border = d.border.name();

    m_browser->setStyleSheet(QString(
        "QTextBrowser { background-color: %1; color: %2;"
        " border: 1px solid %3; }").arg(bg, text, border));

    QString html = QString(
        "<html><head><style>"
        "body { font-family: 'Segoe UI', sans-serif; font-size: 13px;"
        "       background-color: %5; color: %6; }"
        "h2 { font-size: 15px; margin-top: 4px; color: %7; }"
        "ul { margin-left: -6px; }"
        "li { margin: 3px 0; }"
        "a { text-decoration: none; color: %7; }"
        "i { color: %8; }"
        "hr { border: 1px solid %8; }"
        "</style></head><body>"
        "<p><i>%1</i></p>"
        "<h2>%2</h2><ul>%3</ul><hr/>%4"
        "</body></html>")
        .arg(tr("A complete walkthrough of every soundboard feature. "
                "Click a heading below to jump to it."),
             tr("Contents"), toc, bodyHtml,
             bg, text, accent, muted);

    m_browser->setHtml(html);
}
