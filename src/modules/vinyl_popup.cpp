#include "vinyl_popup.h"

#include <QPainter>
#include <QMouseEvent>
#include <QSlider>
#include <QLabel>
#include <QTimer>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QDateTime>
#include <QtMath>
#include <QHideEvent>
#include <QWheelEvent>
#include <QCursor>
#include <QSettings>
#include <cmath>

namespace {
constexpr int kDiscSize = 150;
constexpr int kTickMs   = 33;                 // ~30 fps
// 33.3 rpm -> degrees per tick.
constexpr double kFullSpeed = 33.333 / 60.0 * 360.0 * (kTickMs / 1000.0);
// Linear drag scale: one disc-width (150 px) = one 33 rpm revolution
// = 1.8 s of tape. Right = forward, left = backward.
constexpr double kSecondsPerPx = 1.8 / 150.0;
// Drag threshold (px): below = click, above = scrub gesture.
constexpr int kDragPx = 6;
// Press with no drag for this long = hand lands on the record (grab).
// Short: a real finger stops the platter the instant it lands. Still
// comfortably above a typical click (~85 ms) so clicks stay clicks.
constexpr int kHoldGrabMs = 160;
// One wheel notch (120 units) scrubs this many seconds of tape.
constexpr double kWheelSecondsPerNotch = 0.25;
// Hand-off spin-up (ms): releasing a scratch is a SLIPMAT event - the
// platter is back at speed in a beat, independent of the brake slider
// (that one shapes the cinematic click-stop, a different gesture).
constexpr int kHandOffMs = 250;
}

VinylPopup::VinylPopup(QWidget *parent)
    : QWidget(parent, Qt::Popup | Qt::FramelessWindowHint)
{
    setProperty("isGBSoundboard", true);
    setAttribute(Qt::WA_TranslucentBackground, false);

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(10, kDiscSize + 16, 10, 10);

    auto *row = new QHBoxLayout;
    row->addWidget(new QLabel(tr("Brake"), this));
    m_brakeSlider = new QSlider(Qt::Horizontal, this);
    m_brakeSlider->setRange(100, 3000);
    m_brakeSlider->setValue(800);
    m_brakeSlider->setToolTip(tr(
        "Brake / spin-up duration. Short = hard DJ brake,\n"
        "long = slow cinematic power-down."));
    row->addWidget(m_brakeSlider, 1);
    m_brakeLabel = new QLabel(QStringLiteral("800 ms"), this);
    m_brakeLabel->setMinimumWidth(52);
    m_brakeLabel->setAlignment(Qt::AlignRight);
    row->addWidget(m_brakeLabel);
    lay->addLayout(row);

    // Brake duration persists across restarts (global, all channels).
    {
        QSettings st(QStringLiteral("GameBaiters"), QStringLiteral("Soundboard"));
        int saved = st.value(QStringLiteral("vinyl/brake_ms"), 800).toInt();
        m_brakeSlider->setValue(qBound(100, saved, 3000));
        m_brakeLabel->setText(QString::number(m_brakeSlider->value()) + " ms");
    }
    connect(m_brakeSlider, &QSlider::valueChanged, this, [this](int v){
        m_brakeLabel->setText(QString::number(v) + " ms");
        QSettings st(QStringLiteral("GameBaiters"), QStringLiteral("Soundboard"));
        st.setValue(QStringLiteral("vinyl/brake_ms"), v);
    });

    auto *hint = new QLabel(tr("Drag \xE2\x86\x90\xE2\x86\x92 scratch - Hold = stop\n"
                               "Click = full stop / resume - Wheel = seek"), this);
    hint->setStyleSheet("color: #8aa6c0; font-size: 10px;");
    hint->setAlignment(Qt::AlignCenter);
    lay->addWidget(hint);

    setFixedWidth(kDiscSize + 40);
    setMouseTracking(false);

    m_spinTimer = new QTimer(this);
    m_spinTimer->setInterval(kTickMs);
    connect(m_spinTimer, &QTimer::timeout, this, &VinylPopup::onSpinTick);

    // Hand-stop: press held still past the click window = grab the
    // record (the servo converges to zero = platter stops under the
    // finger). Release spins back up.
    m_holdTimer = new QTimer(this);
    m_holdTimer->setSingleShot(true);
    m_holdTimer->setInterval(kHoldGrabMs);
    connect(m_holdTimer, &QTimer::timeout, this, [this]{
        if (m_holding && !m_dragging && m_tapePhase != 2)
            beginGrab();               // grab without motion
    });

    // Wheel-scrub burst terminator: no notch for 400 ms = hand off.
    m_wheelEndTimer = new QTimer(this);
    m_wheelEndTimer->setSingleShot(true);
    m_wheelEndTimer->setInterval(400);
    connect(m_wheelEndTimer, &QTimer::timeout, this, [this]{
        if (m_wheeling && !m_dragging) {
            m_wheeling = false;
            emit scratchEnded(kHandOffMs);
        }
    });
}

int VinylPopup::brakeMs() const
{
    return m_brakeSlider ? m_brakeSlider->value() : 800;
}

void VinylPopup::popupAt(const QPoint &globalPos)
{
    move(globalPos);
    // Arm BEFORE showing so the ring is ingesting by the first frame.
    emit armChanged(true);
    show();
    raise();
    m_spinTimer->start();
}

void VinylPopup::hideEvent(QHideEvent *e)
{
    // Popup dismissed (outside click / Esc): release any in-flight
    // scratch or wheel-scrub, then disarm the ring. An active brake /
    // stop is NOT cancelled - only the transparent Armed state is.
    m_holdTimer->stop();
    m_wheelEndTimer->stop();
    if (m_holding && m_dragging) {
        endGrab(true);                 // restores the hidden cursor
    } else if (m_wheeling) {
        emit scratchEnded(kHandOffMs);
    }
    m_holding = false;
    m_dragging = false;
    m_wheeling = false;
    unsetCursor();
    m_spinTimer->stop();
    emit armChanged(false);
    QWidget::hideEvent(e);
}

void VinylPopup::setPlaying(bool on)
{
    m_playing = on;
}

void VinylPopup::setTapePhase(int phase)
{
    m_tapePhase = phase;
}

QRect VinylPopup::discRect() const
{
    int x = (width() - kDiscSize) / 2;
    return QRect(x, 10, kDiscSize, kDiscSize);
}

void VinylPopup::onSpinTick()
{
    // While the hand is on the record the disc angle is driven 1:1 by
    // the drag. Position model: a still hand simply stops adding
    // displacement - the audio servo converges and freezes on its own,
    // no explicit zero-rate message needed.
    if (m_dragging) {
        m_spinSpeed = 0.0;
        update();
        return;
    }

    // Target spin speed follows playback + tape phase so the visual
    // matches what the ear hears: braking eases to 0, stopped stays 0,
    // spin-up eases back to full.
    double target;
    if (!m_playing)            target = 0.0;
    else switch (m_tapePhase) {
        case 1:  target = 0.0;        break;   // Braking - ease down
        case 2:  target = 0.0;        break;   // Stopped
        default: target = kFullSpeed; break;   // Idle / SpinUp / CatchUp / Scratch
    }
    // Ease speed; braking/spin-up feel matches the audio ramp roughly.
    double ease = (m_tapePhase == 1 || m_tapePhase == 3) ? 0.08 : 0.25;
    m_spinSpeed += (target - m_spinSpeed) * ease;
    if (m_spinSpeed > 0.001) {
        m_angleDeg += m_spinSpeed;
        if (m_angleDeg >= 360.0) m_angleDeg -= 360.0;
    }
    update();
}

void VinylPopup::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.fillRect(rect(), palette().window());
    p.setPen(QPen(palette().mid().color(), 1));
    p.drawRect(rect().adjusted(0, 0, -1, -1));

    QRect r = discRect();
    QPointF c = r.center();

    p.save();
    p.translate(c);
    p.rotate(m_angleDeg);
    p.translate(-c);

    // Record body.
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(18, 18, 20));
    p.drawEllipse(r);
    // Grooves.
    p.setPen(QPen(QColor(48, 48, 52), 1));
    p.setBrush(Qt::NoBrush);
    for (int i = 1; i <= 6; ++i) {
        int inset = 8 + i * 8;
        p.drawEllipse(r.adjusted(inset, inset, -inset, -inset));
    }
    // Label.
    int li = kDiscSize / 2 - 26;
    QRect label = r.adjusted(li, li, -li, -li);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0xC0, 0x39, 0x2B));
    p.drawEllipse(label);
    // Label marker (shows rotation).
    p.setBrush(QColor(0xF5, 0xE6, 0xC8));
    QRect dot(static_cast<int>(c.x()) - 3,
              label.top() + 4, 6, 6);
    p.drawEllipse(dot);
    // Spindle.
    p.setBrush(QColor(200, 200, 205));
    p.drawEllipse(QRect(static_cast<int>(c.x()) - 3,
                        static_cast<int>(c.y()) - 3, 6, 6));
    p.restore();

    // Stopped badge.
    if (m_tapePhase == 2) {
        p.setPen(QColor(0xF5, 0xE6, 0xC8));
        QFont f = font();
        f.setPixelSize(11);
        f.setBold(true);
        p.setFont(f);
        p.drawText(r, Qt::AlignCenter, tr("STOP"));
    }
}

void VinylPopup::mousePressEvent(QMouseEvent *e)
{
    if (!discRect().contains(e->pos())) {
        QWidget::mousePressEvent(e);
        return;
    }
    m_holding  = true;
    m_dragging = false;
    m_pressPos = e->pos();
    m_restoreGlobal = e->globalPos();   // cursor pops back out here
    m_pressMs  = QDateTime::currentMSecsSinceEpoch();
    // Arm the hand-stop: if the press outlives the click window with
    // no drag, the record gets grabbed (temporary stop). Fires AFTER
    // the click window so a quick click still delivers the full-rate
    // cinematic one-shot brake.
    m_holdTimer->start();
}

void VinylPopup::beginGrab()
{
    // FPS-style pointer capture. The cursor disappears ("the hand
    // sinks into the record") and is pinned to a fixed anchor at the
    // disc centre: every move event is read as RELATIVE motion and the
    // cursor is warped straight back to the pin. No edges, no drift,
    // no leaveEvent - a truly endless, seamless drag.
    m_holdTimer->stop();
    m_dragging = true;
    m_anchorGlobal = mapToGlobal(discRect().center());
    m_lastGlobal   = QCursor::pos();
    setCursor(Qt::BlankCursor);
    if (m_lastGlobal != m_anchorGlobal)
        QCursor::setPos(m_anchorGlobal);
    emit scratchBegan();
    update();
}

void VinylPopup::endGrab(bool emitEnd)
{
    m_dragging = false;
    unsetCursor();
    // The cursor pops back out of the record exactly where the hand
    // grabbed it - as if it never moved.
    QCursor::setPos(m_restoreGlobal);
    if (emitEnd)
        emit scratchEnded(kHandOffMs);
    update();
}

void VinylPopup::mouseMoveEvent(QMouseEvent *e)
{
    if (!m_holding) {
        QWidget::mouseMoveEvent(e);
        return;
    }
    if (!m_dragging) {
        if ((e->pos() - m_pressPos).manhattanLength() < kDragPx)
            return;
        // Threshold crossed: hand lands on the record - scrub mode.
        beginGrab();
        return;
    }

    // Relative pointer mode. Events landing exactly on the anchor are
    // the warp echoes - they only reset the delta chain. Real events
    // chain deltas from the LAST event position (not from the anchor),
    // so motion queued while a warp was still in flight is never
    // double-counted - this was the residual jitter in the edge-warp
    // version of the infinite drag.
    const QPoint gp = e->globalPos();
    if (gp == m_anchorGlobal) {
        m_lastGlobal = gp;
        return;
    }
    const int dx = gp.x() - m_lastGlobal.x();
    m_lastGlobal = gp;
    if (dx != 0) {
        // Linear horizontal scrub: right = forward, left = backward.
        // Position-locked: displacement in px -> seconds of tape; the
        // audio-side platter servo chases the accumulated total.
        emit scratchMoved(static_cast<float>(dx * kSecondsPerPx));

        // Disc feedback: one widget-width of drag = one revolution.
        m_angleDeg += dx * (360.0 / kDiscSize);
        while (m_angleDeg >= 360.0) m_angleDeg -= 360.0;
        while (m_angleDeg < 0.0)    m_angleDeg += 360.0;
        update();
    }
    QCursor::setPos(m_anchorGlobal);
}

void VinylPopup::mouseReleaseEvent(QMouseEvent *e)
{
    Q_UNUSED(e);
    m_holdTimer->stop();
    if (!m_holding) {
        QWidget::mouseReleaseEvent(e);
        return;
    }
    m_holding = false;
    if (m_dragging) {
        // Hand off the record: spin back up to normal speed.
        endGrab(true);
        return;
    }
    // No drag and inside the click window = a click. Stopped disc
    // resumes; spinning disc gets the one-shot timed brake.
    if (m_tapePhase == 2)
        emit resumeRequested(brakeMs());
    else
        emit oneShotRequested(brakeMs());
}

void VinylPopup::wheelEvent(QWheelEvent *e)
{
    // Wheel = infinite scrub with the cursor: each notch nudges the
    // tape +/- kWheelSecondsPerNotch. First notch grabs the record,
    // 400 ms without notches releases it (spin-up).
    double notches = e->angleDelta().y() / 120.0;
    if (notches == 0.0) { e->ignore(); return; }
    if (!m_dragging && !m_wheeling) {
        m_wheeling = true;
        emit scratchBegan();
    }
    emit scratchScrolled(static_cast<float>(notches * kWheelSecondsPerNotch));
    m_angleDeg += notches * kWheelSecondsPerNotch / 1.8 * 360.0;
    while (m_angleDeg >= 360.0) m_angleDeg -= 360.0;
    while (m_angleDeg < 0.0)    m_angleDeg += 360.0;
    update();
    if (m_wheeling) m_wheelEndTimer->start();
    e->accept();
}

void VinylPopup::leaveEvent(QEvent *e)
{
    // Mouse escaped mid-scrub = hand off (the platter must never stay
    // grabbed because the cursor left the popup). The anchor pin keeps
    // the cursor glued to the disc centre during normal use, so this
    // only fires on genuine escapes (e.g. alt-tab).
    m_holdTimer->stop();
    if (m_holding && m_dragging) {
        m_holding = false;
        endGrab(true);
    }
    m_holding = false;
    QWidget::leaveEvent(e);
}
