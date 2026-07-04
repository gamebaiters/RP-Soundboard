// Channel - composes VolumeControl + FxPanel + WaveformPlayer for a single
// playback slot. Owns no audio engine, only its UI state. The host page wires
// channel signals to the Sampler (and reverse) when this module is mounted.

#pragma once

#include <QWidget>
#include "volume_control.h"
#include "fx_panel.h"
#include "waveform_player.h"
#include "../dsp/SandboxState.h"

class QPushButton;
class QLineEdit;
class QLabel;
class ChannelMeter;
class ChannelSandboxDialog;

struct ChannelState {
    int          volumeLocal  = 100;
    int          volumeRemote = 100;
    bool         volumesLinked= false;
    int          pitch        = 0;
    int          speed        = 0;
    int          reverb       = 0;
    bool         fxSync       = false;
    QString      filename;
    double       playbackPos  = 0.0;
    SandboxState sandbox;

    QByteArray  toJson() const;
    static bool fromJson(const QByteArray &data, ChannelState &out);
};

class Channel : public QWidget {
    Q_OBJECT
public:
    explicit Channel(int channelId, QWidget *parent = nullptr);

    int             channelId() const { return m_id; }
    // Reassign the channel's positional id after a sibling channel was
    // removed - keeps m_id in lock-step with the channel's index in
    // MainPage::m_channels so persistence + sandbox dialogs stay in
    // sync. Refreshes the sandbox dialog window title in passing.
    void            setChannelId(int newId);
    ChannelState    state()     const;
    void            applyState(const ChannelState &s);

    VolumeControl  *volume()   { return m_volume; }
    FxPanel        *fx()       { return m_fx; }
    WaveformPlayer *waveform() { return m_wave; }

    void            setRemovable(bool on);
    void            setTitle(const QString &title);
    QString         title() const;
    // Hide / show the per-channel FX panel + its separator. Driven by
    // the global "Enable custom FX" master switch.
    void            setFxVisible(bool on);
    // Hide / show the WaveformPlayer (waveform paint + transport row).
    // Lets the user run a compact UI when they only need the sliders.
    void            setWaveformVisible(bool on);
    // Re-apply theme-derived inline stylesheets on the frame, remove
    // button and title edit. Called when the active theme changes so
    // these widgets follow the new palette.
    void            refreshTheme();

    // ---- Audio sandbox per-channel ----
    void                setSandboxState(const SandboxState &s);
    const SandboxState &sandboxState() const { return m_sandbox; }
    // Open / focus the sandbox dialog for this channel. Lazy-built.
    void                openSandboxDialog();
    // Push a peak L/R reading into the meter (called by a wiring-layer
    // QTimer that polls Sampler::getSlotPeak).
    void                setMeterPeak(float l, float r);
    // Show / hide the meter widget (driven by the global "Show audio
    // meter on channels" setting).
    void                setMeterVisible(bool on);
    // Switch the per-channel LED visualizer between horizontal and
    // vertical orientation. Driven by the global "Vertical LED
    // visualizer" setting. Layout re-flows so the sliders on the left
    // fill the vacated space and no dead gap appears.
    void                setMeterVertical(bool on);
    // Show / hide the -10s / -5s / +5s / +10s skip buttons on this
    // channel's waveform transport. Driven by the global "Show skip
    // buttons" setting.
    void                setSkipButtonsVisible(bool on);
    // Show / hide the tape-stop vinyl button on the transport row.
    // Driven by the global "Show vinyl (tape stop) button" setting.
    void                setVinylButtonVisible(bool on) {
        if (m_wave) m_wave->setVinylButtonVisible(on);
    }
    // Programmatic remove trigger. Emits removeChannelRequested with
    // the channel's own id so external code (multi-channel infinity
    // auto-cleanup path) can request removal without touching the
    // private m_removeBtn.
    void                requestRemove() { emit removeChannelRequested(m_id); }
    // Show / hide the sandbox button (driven by the global "Enable
    // audio sandbox feature" setting).
    void                setSandboxFeatureEnabled(bool on);
    void                setExportVisible(bool on);
    // Notify the sandbox dialog (if open) that the channel title
    // changed, so the dialog window title stays in sync.
    void                pushTitleToSandboxDialog();
    // Push a rolling DSP CPU% reading into the sandbox dialog when it
    // is currently visible. No-op when the dialog has not been opened
    // yet (lazy build).
    void                pushSandboxCpu(double pct);
    // Push the channel's current peak level into the sandbox dialog so
    // the EQ band widgets animate. No-op when dialog not visible.
    void                pushSandboxLevel(float peakL, float peakR);
    // Per-band spectrum levels (16 floats, 0..1) from the slot's FFT.
    // The dialog forwards them straight to the matching band widgets.
    void                pushSandboxEqLevels(const float bands[16]);

signals:
    void stateChanged(int channelId);
    void addChannelRequested(int afterChannelId);
    void removeChannelRequested(int channelId);
    void titleChanged(int channelId, const QString &title);
    // Drag a SoundButton from the grid onto this Channel = load that
    // sound into this slot. Wiring stops current playback, plays new,
    // immediately pauses so the user can hit play when ready.
    void soundDroppedFromButton(int channelId, int buttonIdx);
    // Sandbox state changed via the per-channel dialog. Wiring layer
    // forwards this to Sampler::setSlotSandboxState + persists to the
    // INI via ChannelStatePersistence.
    void sandboxStateChanged(int channelId, const SandboxState &s);
    void sandboxResetRequested(int channelId);
    void exportRequested(int channelId);

protected:
    void dragEnterEvent(class QDragEnterEvent *e) override;
    void dragMoveEvent (class QDragMoveEvent  *e) override;
    void dropEvent     (class QDropEvent      *e) override;
    void resizeEvent   (class QResizeEvent    *e) override;

private slots:
    void onAnyChange();
    void onTitleEditFinished();

private:
    // Drives the meter width directly from the channel width so the
    // meter yields space to the volume / FX controls first.
    void updateMeterWidth();

private:
    int             m_id;
    VolumeControl  *m_volume;
    FxPanel        *m_fx;
    WaveformPlayer *m_wave;
    QPushButton    *m_addBtn;
    QPushButton    *m_removeBtn;
    QLineEdit      *m_titleEdit;
    class QFrame   *m_fxSeparator = nullptr;
    class QFrame   *m_frame = nullptr;

    // Sandbox per-channel
    SandboxState                  m_sandbox;
    QPushButton                  *m_sandboxBtn = nullptr;
    class QCheckBox              *m_sandboxEnableCheck = nullptr;
    ChannelMeter                 *m_meter = nullptr;
    ChannelSandboxDialog         *m_sandboxDialog = nullptr;
    QPushButton                  *m_exportBtn = nullptr;
    bool                          m_sandboxFeatureEnabled = true;
};
