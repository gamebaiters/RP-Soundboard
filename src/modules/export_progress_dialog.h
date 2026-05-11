// ExportProgressDialog - non-modal progress card shown while
// AudioExporter encodes a WAV. Tracks percentage live, then morphs
// into a success / failure card with a single dismiss button.

#pragma once

#include <QDialog>
#include <QString>

class QLabel;
class QProgressBar;
class QPushButton;
class QFrame;

class ExportProgressDialog : public QDialog {
    Q_OBJECT
public:
    ExportProgressDialog(const QString &outputFile, QWidget *parent = nullptr);

    // Drive percent 0..100 while encoding.
    void setProgress(int percent);
    // Transition to terminal "completed" / "failed" state. The dialog
    // stays open until the user clicks the dismiss button.
    void setFinished(bool ok, const QString &error);

signals:
    // Emitted when the user clicks Cancel during encoding. The wiring
    // layer forwards this to AudioExporter::requestInterruption().
    void cancelRequested();

private:
    void applyTheme();

    QString       m_outputFile;
    QFrame       *m_card     = nullptr;
    QLabel       *m_icon     = nullptr;
    QLabel       *m_title    = nullptr;
    QLabel       *m_filename = nullptr;
    QProgressBar *m_bar      = nullptr;
    QLabel       *m_status   = nullptr;
    QPushButton  *m_cancel   = nullptr;
    QPushButton  *m_dismiss  = nullptr;
    bool          m_finished = false;
};
