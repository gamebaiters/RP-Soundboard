#pragma once

#include <QWidget>

class QSlider;
class QLabel;
class QTimer;

// Tape-stop vinyl popup (D1).
//
// A small frameless popup anchored to the channel's vinyl button. It
// paints a spinning record (33 rpm while the channel plays); the disc
// itself is the control:
//
//   HOLD the disc  -> brake engages (rate ramps to 0); release spins
//                     back up. DJ "brake" gesture.
//   CLICK the disc -> one-shot full tape stop; the slot auto-pauses
//                     when the ramp reaches zero. Click again to
//                     spin up + resume.
//
// A slider below picks the brake/spin-up duration (100..3000 ms).
// The popup polls Sampler::tapeState at 30 Hz to sync the animation
// (braking decelerates the spin, stopped freezes it, spin-up
// accelerates).
class VinylPopup : public QWidget
{
    Q_OBJECT
public:
    explicit VinylPopup(QWidget *parent = nullptr);

    // Anchor just below the given global position (button corner).
    void popupAt(const QPoint &globalPos);

    // Live playback feedback from the wiring: whether the channel is
    // audible and the current TapeStop::Phase (0=Idle 1=Braking
    // 2=Stopped 3=SpinUp 4=CatchUp).
    void setPlaying(bool on);
    void setTapePhase(int phase);

    int brakeMs() const;

signals:
    // Click (no drag) = one-shot full stop (or resume when stopped).
    void oneShotRequested(int brakeMs);
    void resumeRequested(int spinMs);
    // Scratch gesture: press+drag on the disc. deltaSeconds is the
    // angular displacement converted to SECONDS of tape at 33 rpm
    // (1 revolution = 1.8 s), positive = forward. The audio side runs
    // a position servo, so the sound tracks the disc 1:1.
    void scratchBegan();
    void scratchMoved(float deltaSeconds);
    // Wheel notches: same seconds-of-tape unit, but the wiring routes
    // the overflow to decoder seeks (infinite fast-forward / rewind).
    // The DRAG gesture (scratchMoved) must never seek - it clamps at
    // the ring edges like a real record.
    void scratchScrolled(float deltaSeconds);
    void scratchEnded(int spinMs);
    // Popup shown/hidden: the wiring arms/disarms the tape ring so
    // history is already ingested when the first drag lands.
    void armChanged(bool armed);

protected:
    void paintEvent(QPaintEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void wheelEvent(class QWheelEvent *e) override;
    void leaveEvent(QEvent *e) override;
    void hideEvent(class QHideEvent *e) override;

private slots:
    void onSpinTick();

private:
    QRect discRect() const;
    // Enter FPS-style pointer capture: the cursor vanishes ("the hand
    // sinks into the record"), gets pinned to a fixed anchor and only
    // RELATIVE motion is read - endless drag with zero drift.
    void beginGrab();
    // Leave pointer capture: hand off the record, cursor reappears at
    // the exact spot it was grabbed from.
    void endGrab(bool emitEnd);

    QSlider *m_brakeSlider = nullptr;
    QLabel  *m_brakeLabel  = nullptr;
    QTimer  *m_spinTimer   = nullptr;
    // Fires 220 ms after a press with no drag: the hand has landed on
    // the record - engage the grab (platter stops under the finger).
    QTimer  *m_holdTimer   = nullptr;
    // Ends a wheel-scrub burst: no wheel notch for 400 ms = hand off.
    QTimer  *m_wheelEndTimer = nullptr;

    bool   m_playing   = false;
    int    m_tapePhase = 0;
    double m_angleDeg  = 0.0;
    double m_spinSpeed = 0.0;     // deg per tick, eased toward target
    bool   m_holding   = false;   // mouse down on the disc
    bool   m_dragging  = false;   // grab engaged (drag or hold)
    bool   m_wheeling  = false;   // wheel-scrub burst in flight
    QPoint m_pressPos;
    qint64 m_pressMs   = 0;
    // Pointer-capture state (all in GLOBAL coordinates).
    QPoint m_anchorGlobal;        // fixed pin the cursor is warped to
    QPoint m_lastGlobal;          // last real event pos (delta chain)
    QPoint m_restoreGlobal;       // where the cursor pops back out
};
