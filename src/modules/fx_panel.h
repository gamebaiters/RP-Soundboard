// FxPanel - Pitch / Speed / Reverb sliders with optional pitch<->speed sync
// and a reset button. Pure UI; emits value-changed signals.

#pragma once

#include <QWidget>

class QSlider;
class QLabel;
class QToolButton;

class FxPanel : public QWidget {
    Q_OBJECT
public:
    explicit FxPanel(QWidget *parent = nullptr);

    int  pitch()   const;
    int  speed()   const;
    int  reverb()  const;
    bool sync()    const;

    // Range goes ±100 so the 3^(v/100) factor reaches 3.0x at +100 and
    // 0.33x (= 1/3x) at -100 -- the natural "+/-3x" feel users expect.
    static constexpr int kPitchMin  = -100;
    static constexpr int kPitchMax  =  100;
    static constexpr int kSpeedMin  = -100;
    static constexpr int kSpeedMax  =  100;
    static constexpr int kReverbMin =   0;
    static constexpr int kReverbMax = 100;

public slots:
    void setPitch(int v);
    void setSpeed(int v);
    void setReverb(int v);
    void setSync(bool on);
    void resetAll();
    // Re-apply theme-derived inline stylesheets on m_sync and m_reset.
    // These two buttons need their own per-widget stylesheet (Qt 5
    // cascade misses subclass widgets created late) so they have to be
    // re-pushed every time the theme changes.
    void refreshTheme();

signals:
    void pitchChanged(int v);
    void speedChanged(int v);
    void reverbChanged(int v);
    void syncChanged(bool on);
    void resetClicked();

private slots:
    void onPitchMoved(int v);
    void onSpeedMoved(int v);
    void onReverbMoved(int v);
    void onSyncToggled(bool on);

private:
    QSlider     *m_pitch;
    QSlider     *m_speed;
    QSlider     *m_reverb;
    QLabel      *m_pitchLabel;
    QLabel      *m_speedLabel;
    QLabel      *m_reverbLabel;
    QToolButton *m_sync;
    QToolButton *m_reset;
    bool         m_internalSync;
};
