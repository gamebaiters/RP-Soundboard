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
        "<p>The soundboard is a TeamSpeak 3 plugin. Open it from "
        "<b>Plugins</b> &rarr; <b>Open GameBaiters Soundboard</b>. The window "
        "is independent from TS3 and can be moved, resized or maximised.</p>"
        "<p>You must be <b>connected to a server</b> for sound to be sent into "
        "your voice channel. While disconnected the window dims and shows a "
        "notice, but you can still arrange buttons, edit effects and tune "
        "the layout.</p>"
        "<p>The window has four areas:</p>"
        "<ul>"
        "<li><b>Sound button grid</b> at the top &mdash; assign / fire sounds, "
        "drag to rearrange, filter via the search bar above.</li>"
        "<li><b>Channels</b> in the middle &mdash; one playback lane each, "
        "stacked vertically.</li>"
        "<li><b>Profile selector</b> P1&ndash;P4 &mdash; switch between four "
        "independent layouts.</li>"
        "<li><b>Control bar</b> at the bottom &mdash; add / remove channel, "
        "stop / pause all, settings.</li>"
        "</ul>") });

    s.append({ "buttons", tr("2. Sound buttons"), tr(
        "<p>Every grid cell is a button. Three ways to assign a sound:</p>"
        "<ul>"
        "<li><b>Drag &amp; drop</b> an audio file from Explorer / Finder onto "
        "a cell.</li>"
        "<li><b>Right-click</b> a cell &rarr; <b>Edit</b> to open the full "
        "button editor (file picker, label, colour, image, crop, hotkey, "
        "looping, per-button FX).</li>"
        "<li>Drag an audio file directly onto a <b>channel</b> instead of a "
        "button to play it immediately without saving a cell.</li>"
        "</ul>"
        "<p><b>Supported formats:</b> MP3, WAV, OGG, FLAC, AAC, M4A, MP4, MKV, "
        "AVI, WMA, FLV, OPUS &mdash; anything FFmpeg can decode.</p>"
        "<p><b>Three button types:</b></p>"
        "<ul>"
        "<li><b>Audio</b> &mdash; plays a sound file. Default for any drag &amp; "
        "drop.</li>"
        "<li><b>Macro</b> &mdash; restores a saved snapshot of every channel "
        "at once (see section 13).</li>"
        "<li><b>Special</b> &mdash; bound actions such as stop-all.</li>"
        "</ul>"
        "<p><b>Customising a button:</b> in the editor you can set a custom "
        "label (overrides the filename), a custom background colour, a "
        "background image, looping, per-button volume, a TeamSpeak hotkey, and "
        "a crop range (see the next section). Drag buttons in the grid to "
        "rearrange them. The <b>search bar</b> above the grid filters by "
        "label. Grid size (rows &times; columns) is set in Settings &rarr; "
        "<b>Layout</b>.</p>") });

    s.append({ "crop", tr("3. Cropping and waveform editing"), tr(
        "<p>Every sound can be <b>cropped</b> to play only a slice of the "
        "file. Two ways to do it:</p>"
        "<ul>"
        "<li><b>Button editor:</b> set start / end in seconds (or %) in the "
        "<b>Crop</b> group.</li>"
        "<li><b>Waveform right-click:</b> while a channel is showing a sound, "
        "right-click the waveform &rarr; <b>Set start here</b> / <b>Set end "
        "here</b>. The change is applied live to the running playback AND "
        "saved back to the originating button.</li>"
        "</ul>"
        "<p>Coloured <b>start / end marker lines</b> with a dim overlay show "
        "the crop range right on the waveform. The cursor is clamped inside "
        "the markers &mdash; skip / seek can not escape the crop.</p>"
        "<p>The crop marker overlay can be hidden via Settings &rarr; "
        "<b>Channels</b> &rarr; <b>Show crop markers</b>.</p>") });

    s.append({ "play", tr("4. Playing and transport"), tr(
        "<p>Click a button to play its sound on the next free channel. The "
        "<b>waveform view</b> shows playback progress &mdash; click anywhere "
        "on it to seek.</p>"
        "<p><b>Transport controls</b> (per channel):</p>"
        "<ul>"
        "<li><b>Play / Pause / Stop</b></li>"
        "<li><b>Skip</b> &plusmn;5 s and &plusmn;10 s</li>"
        "<li><b>Loop</b> &mdash; restarts the sound at the crop start when it "
        "reaches the end</li>"
        "<li><b>Reverse</b> &mdash; plays the sound backwards in real time. "
        "Toggle it mid-playback and the direction flips instantly; pitch / "
        "speed / reverb / loop / crop markers all behave exactly as in "
        "forward play. From v2.2.9 the reverse start is decoded in 0.5 s "
        "chunks around the current cursor, so the flip is instant even on "
        "multi-minute files &mdash; no more freeze.</li>"
        "</ul>"
        "<p><b>Global controls</b> in the bottom bar:</p>"
        "<ul>"
        "<li><b>Stop all</b> / <b>Pause all</b> &mdash; act on every channel.</li>"
        "<li><b>Reset channels</b> (in Settings) &mdash; clears volume / FX / "
        "files on every channel after a confirmation prompt.</li>"
        "</ul>") });

    s.append({ "channels", tr("5. Channels"), tr(
        "<p>A <b>channel</b> is one independent playback lane: it has its own "
        "file, volume, FX, waveform, transport, peak meter and audio sandbox. "
        "Add channels with <b>+ Add channel</b>; the first channel is always "
        "present.</p>"
        "<p>Channel titles are editable &mdash; click the title to rename. "
        "The <b>peak meter</b> shows level (cyan = normal, red = clipping) and "
        "the LED columns above each EQ band visualise the live per-band "
        "energy when the EQ is open.</p>"
        "<p>Each channel's state (volume, FX, sandbox, current sound, position, "
        "loop / reverse toggles) is saved and restored across restarts when "
        "<b>Restore last session on startup</b> is enabled in Settings.</p>"
        "<p>The <b>Reverse</b> button on a channel is sticky: every new sound "
        "you fire on that channel inherits the reverse direction. Toggle it "
        "off to go back to forward play.</p>") });

    s.append({ "volume", tr("6. Volume control"), tr(
        "<p>Every channel has <b>two</b> volume sliders:</p>"
        "<ul>"
        "<li><b>Local</b> &mdash; how loud <i>you</i> hear the sound.</li>"
        "<li><b>Remote</b> &mdash; how loud <i>others</i> on the server hear "
        "it. This is the value the TS3 client mixes into the voice stream.</li>"
        "</ul>"
        "<p>The <b>Link</b> toggle moves both sliders together. A <b>Global "
        "volume</b> master at the top scales everything before per-channel "
        "Local / Remote. Each button also has its own per-button volume in "
        "its editor &mdash; it stacks on top of the channel volume.</p>"
        "<p><b>Earrape protection</b> in Settings clips your LOCAL output at a "
        "safe ceiling, so an accidental loud clip can never deafen you. It "
        "never touches the Remote stream.</p>") });

    s.append({ "fx", tr("7. FX panel (pitch / speed / reverb)"), tr(
        "<p>The compact <b>FX panel</b> on each channel offers three quick, "
        "real-time effects:</p>"
        "<ul>"
        "<li><b>Pitch</b> &mdash; shift pitch independently of speed "
        "(0.33&times;&ndash;3.0&times;).</li>"
        "<li><b>Speed</b> &mdash; change playback rate independently of pitch.</li>"
        "<li><b>Reverb</b> &mdash; quick room ambience (0&ndash;100).</li>"
        "</ul>"
        "<p>The <b>Sync</b> toggle locks pitch and speed together for a "
        "chipmunk / slow-motion effect.</p>"
        "<p>With <b>Remember pitch / speed / reverb per channel</b> enabled in "
        "Settings, these values persist across restarts. With <b>Enable custom "
        "FX</b> off in Settings the panel is hidden &mdash; useful if you only "
        "want clean playback.</p>"
        "<p>The FX panel is the <i>quick</i> effects strip. For the deep "
        "14-effect chain, open the per-channel <b>Audio sandbox</b> (next "
        "section).</p>") });

    s.append({ "sandbox", tr("8. The audio sandbox"), tr(
        "<p>The <b>audio sandbox</b> is a per-channel rack of <b>14 DSP "
        "effects</b> arranged in a re-orderable pipeline. Open it from the "
        "channel's sandbox button.</p>"
        "<p>A single <b>master switch</b> at the top enables or bypasses the "
        "whole chain at zero CPU cost when off &mdash; every value is kept so "
        "you can A/B instantly.</p>"
        "<p>The <b>Pipeline order</b> bar near the top shows the chain as "
        "coloured blocks in processing order:</p>"
        "<ul>"
        "<li><b>Drag</b> a block to move it earlier / later in the chain.</li>"
        "<li><b>Click</b> a block (without dragging) to scroll to that effect's "
        "panel and open its accordion.</li>"
        "<li><b>Paulstretch</b> is pinned first and is not draggable &mdash; "
        "it runs on a separate streaming feed before the rest of the chain.</li>"
        "<li><b>Reset order</b> restores the default chain (effect values are "
        "kept).</li>"
        "</ul>"
        "<p>The panel list below mirrors the pipeline order. Use the "
        "<b>search box</b> to filter by name. Each module has its own enable "
        "toggle and a small reset button (per-module defaults).</p>"
        "<p>The <b>Fold output to mono</b> checkbox collapses the stereo "
        "image and is always applied LAST &mdash; it is not a reorderable "
        "module.</p>"
        "<p>The whole sandbox state is saved per channel and travels inside "
        "sandbox presets, pipeline order included.</p>") });

    s.append({ "spatial", tr("9. 3D / HRTF spatial audio"), tr(
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
        "adds room reflections that push the sound out of your head.</p>"
        "<p><b>Engine choice (Classic vs Leia):</b> the 3D modes can be "
        "rendered by two engines, selected per channel:</p>"
        "<ul>"
        "<li><b>Classic</b> &mdash; the original Brown-Duda parametric model. "
        "Lightweight, zero dependencies, the original soundboard sound.</li>"
        "<li><b>Leia</b> &mdash; measured-HRTF convolution using a MIT KEMAR "
        "SOFA dataset, plus a shoebox-room early-reflection engine. Much more "
        "convincing externalisation, especially through headphones.</li>"
        "</ul>"
        "<p>Leia exposes its own controls: <b>Reflections</b> on/off + level, "
        "<b>Room size</b>, <b>Room type</b> (13 presets ranging from Studio to "
        "Cathedral to Underwater &mdash; each preset has a unique Schroeder "
        "diffuse-tail signature), <b>Clarity</b> (early/late balance) and "
        "<b>Width</b>. Switching from Classic to Leia is instant &mdash; the "
        "audio thread keeps running while the SOFA loads in the background.</p>") });

    s.append({ "stretch", tr("10. Paulstretch"), tr(
        "<p><b>Paulstretch</b> is an extreme time-stretch (1&times;&ndash;50&times;) "
        "that keeps pitch while slowing the sound into a wide, diffuse drone. "
        "It is the first module in the sandbox list and its pipeline position "
        "is fixed.</p>"
        "<p><b>Factor</b> sets how much slower it plays; <b>Window</b> sets the "
        "FFT analysis size (bigger = smoother and more 'frozen', smaller = "
        "grittier and more rhythmic).</p>"
        "<p>Paulstretch runs on a separate streaming feed before the rest of "
        "the chain. While it is on the server receives a clean, stretched "
        "stream &mdash; the same audio you hear locally.</p>") });

    s.append({ "effects", tr("11. The DSP effects (the other 13)"), tr(
        "<p>Listed in the default pipeline order:</p>"
        "<ul>"
        "<li><b>16-band EQ</b> &mdash; ISO 2/3-octave graphic equaliser, "
        "&plusmn;12 dB per band, with live per-band LED level meters and "
        "Save / Load EQ presets. Right-click a band &rarr; reset / +12 / -12 dB. "
        "Mouse wheel adjusts by 1 dB (Shift = 3 dB).</li>"
        "<li><b>Compressor</b> &mdash; evens out dynamics (threshold, ratio, "
        "attack, release, knee, makeup gain).</li>"
        "<li><b>Saturator</b> &mdash; adds warmth / grit; soft, tube, tape or "
        "hard-clip modes.</li>"
        "<li><b>Spatial</b> &mdash; see section 9.</li>"
        "<li><b>Chorus</b> &mdash; thickens the sound with detuned voices.</li>"
        "<li><b>Flanger</b> &mdash; classic sweeping jet effect.</li>"
        "<li><b>Flangus</b> &mdash; flanger / chorus hybrid with multi-voice "
        "spread.</li>"
        "<li><b>Phaser</b> &mdash; sweeping notch filter, up to 12 stages.</li>"
        "<li><b>Delay</b> &mdash; echo up to 3 s with damping and optional "
        "ping-pong.</li>"
        "<li><b>Reverb</b> &mdash; algorithmic room ambience (in addition to "
        "Leia's measured reflections).</li>"
        "<li><b>Limiter</b> &mdash; brick-wall limiter, compressor or noise "
        "gate.</li>"
        "<li><b>Bitcrusher</b> &mdash; reduces bit depth and sample rate for a "
        "lo-fi sound.</li>"
        "<li><b>Generation Loss</b> &mdash; simulates repeated re-encoding "
        "(the 'deep-fried' effect).</li>"
        "</ul>"
        "<p>Each effect has its own enable toggle, per-module reset button and "
        "is fully drag-reorderable in the pipeline. Live state persists across "
        "restarts.</p>") });

    s.append({ "extras", tr("12. Extras: ducking, random, normalize"), tr(
        "<p>The sandbox also includes a few global features that are not "
        "ordered as pipeline modules:</p>"
        "<ul>"
        "<li><b>Auto normalize (LUFS)</b> &mdash; EBU R128 single-pass "
        "loudness normalisation targeting -16 LUFS integrated / -1 dBTP. "
        "Levels every clip without you having to ride the volume.</li>"
        "<li><b>Random pitch jitter</b> &mdash; on every play (and every loop "
        "iteration) the pitch is shifted by a random number of cents within "
        "the range you set. Great for spam buttons that should never sound "
        "identical twice.</li>"
        "<li><b>Sidechain ducking</b> &mdash; while this channel is "
        "transmitting, it attenuates every OTHER channel by N dB. Smooth "
        "attack / release so the duck doesn't pump.</li>"
        "</ul>") });

    s.append({ "presets", tr("13. Presets, copy / paste"), tr(
        "<p>You can save and reload named presets:</p>"
        "<ul>"
        "<li><b>EQ presets</b> &mdash; just the 16 EQ bands.</li>"
        "<li><b>Sandbox presets</b> &mdash; every effect, every parameter and "
        "the pipeline order.</li>"
        "</ul>"
        "<p>Presets are stored in a per-user INI so they survive reinstalls "
        "and travel between profiles.</p>"
        "<p><b>Copy / Paste</b> buttons in the sandbox dialog move a complete "
        "setup between channels through the clipboard as a short text string "
        "&mdash; perfect for matching all channels to one chain.</p>") });

    s.append({ "macros", tr("14. Macros"), tr(
        "<p>A <b>macro button</b> freezes the state of <i>every</i> channel "
        "(volumes, FX, files, full sandbox state) into one button. Pressing it "
        "later restores that whole snapshot in one click.</p>"
        "<p>Create a macro by right-clicking a free cell &rarr; <b>Make "
        "macro</b>. Macro buttons carry a visual marker so they're easy to "
        "spot.</p>"
        "<p><b>Undo macro</b> (previously 'Restore pre-macro') reverts to the "
        "exact state the channels were in just before the macro fired &mdash; "
        "use it when you want to A/B between your live setup and a saved "
        "snapshot.</p>") });

    s.append({ "profiles", tr("15. Profiles"), tr(
        "<p>The soundboard has <b>4 independent profiles</b> (P1&ndash;P4), "
        "each with its own button grid, grid size, macros and per-profile "
        "settings. Switch with the P1&ndash;P4 buttons in the top bar or via "
        "a TS3 hotkey.</p>"
        "<p>Profiles can be <b>exported as JSON</b> to back up or share. The "
        "exported file is human-readable and contains every button, every "
        "label, every colour and every macro. Importing a JSON profile is "
        "lossless.</p>"
        "<p>The currently selected profile is automatically persisted &mdash; "
        "the next start opens the same profile.</p>") });

    s.append({ "hotkeys", tr("16. Hotkeys"), tr(
        "<p>Any button can be bound to a <b>TS3 hotkey</b>. Open the button "
        "editor &rarr; <b>Set hotkey...</b> and follow the TS3 prompt to "
        "assign a key combo.</p>"
        "<p>Enable <b>Show hotkeys on buttons</b> in Settings to display the "
        "binding as a small label on each button &mdash; very handy when you "
        "have dozens of bindings.</p>"
        "<p><b>Disable hotkeys</b> in Settings temporarily ignores all "
        "soundboard hotkeys without losing the bindings &mdash; useful when "
        "you need to type something or play a game without firing sounds by "
        "accident.</p>") });

    s.append({ "export", tr("17. Audio export"), tr(
        "<p><b>Audio export</b> renders a sound to a 48 kHz stereo file with "
        "the <i>entire</i> signal chain baked in: pitch, speed, reverb, the "
        "full sandbox DSP chain (including Paulstretch).</p>"
        "<p>Supported output formats: <b>WAV</b> (PCM 16-bit), <b>FLAC</b> "
        "(lossless), <b>OGG Vorbis</b> and <b>AAC-M4A</b>. The format is "
        "picked automatically from the filename extension you choose in the "
        "save dialog.</p>"
        "<p>Export runs in a background thread with a custom-painted progress "
        "bar; closing the dialog cancels the job cleanly. The exported file "
        "is bit-identical to what you would hear on playback.</p>") });

    s.append({ "theme", tr("18. Theme"), tr(
        "<p>In Settings &rarr; <b>Theme</b> you can recolour the soundboard "
        "with three colours (accent, waveform, background) plus a contrast "
        "slider. Over a dozen surface colours (button hover, dim text, panel "
        "border, etc.) are derived automatically from those three and applied "
        "live.</p>"
        "<p>The theme is scoped to the soundboard &mdash; it never leaks into "
        "the TeamSpeak client window.</p>") });

    s.append({ "language", tr("19. Language"), tr(
        "<p>The interface auto-selects <b>Italian</b> when the system locale "
        "is Italian, English otherwise. You can override the language in "
        "Settings &rarr; <b>Language</b> at any time. The change applies on "
        "the next plugin start (a quick TS3 reload is enough).</p>") });

    s.append({ "settings", tr("20. Settings overview"), tr(
        "<p>The Settings window collects every global option:</p>"
        "<ul>"
        "<li><b>Layout</b> &mdash; grid rows / columns, hide waveform "
        "(compact view), audio meter visibility, hotkey display, crop markers, "
        "channel headers.</li>"
        "<li><b>Channels</b> &mdash; remember FX per channel, restore last "
        "session, link new channels' settings to the first channel.</li>"
        "<li><b>Audio</b> &mdash; earrape protection, custom FX, audio sandbox, "
        "show export button.</li>"
        "<li><b>Logs</b> &mdash; write debug log file (see "
        "<b>Plugins menu &rarr; Show plugin log</b>).</li>"
        "<li><b>Reset</b> &mdash; one-shot reset of channels, volumes, FX, "
        "files or sandbox settings (with a confirmation prompt).</li>"
        "<li><b>Theme</b> &mdash; three colours + contrast.</li>"
        "<li><b>Language</b> &mdash; auto / en / it.</li>"
        "</ul>") });

    s.append({ "updates", tr("21. Updates and About"), tr(
        "<p>The plugin checks for updates on startup. Run a manual check "
        "from <b>Plugins</b> &rarr; <b>Check for update</b>. When a newer "
        "version exists it is downloaded from GitHub Releases for your "
        "platform (Windows / Linux / macOS) and a TS3 restart applies it.</p>"
        "<p>From the <b>About</b> dialog you can also open this guide and the "
        "<b>Version history</b> &mdash; the full change log of every "
        "documented release, browsable per-version on a split view.</p>") });

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
