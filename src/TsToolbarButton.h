#pragma once

class QWidget;

// UNOFFICIAL host-toolbar integration: a checkable soundboard button
// injected into the TeamSpeak 3 client's own main toolbar (next to
// mute / away). The TS3 client is a Qt application and the plugin runs
// in-process with the same Qt runtime, so the button is added with
// plain QToolBar::addWidget on the toolbar found in the client's main
// window (community-proven technique; addAction is filtered by the
// client's ImprovedToolBar subclass, an added WIDGET is not).
//
// Fully defensive:
//  - if no toolbar is found (client redesign), the plugin silently
//    behaves as before - the Plugins menu keeps working;
//  - the button and every helper object are torn down in sb_kill
//    BEFORE the DLL unloads (a leftover child widget in the host would
//    call a dead vtable on client exit - same class of bug as the
//    ghost-crash / deferred-callback fixes);
//  - the host style is never touched.
namespace TsToolbarButton {

// Try to install the button (idempotent). Retries a few times on a
// timer - the client main window may not be fully built during plugin
// init. Call from the GUI thread.
void install();

// Settings gate ("Show soundboard button in the TeamSpeak toolbar").
// false = the overlay is removed immediately; true = re-installed.
void setUserEnabled(bool on);

// Tell the button which soundboard window to mirror: checked follows
// the window's Show/Hide events; unchecking hides the window. Call
// whenever the window is (re)created.
void watchWindow(QWidget *w);

// Remove the button from the host toolbar and delete every object this
// module created. MUST run in sb_kill while Qt is still alive.
void remove();

}
