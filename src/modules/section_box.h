// SectionBox - a QGroupBox that can be folded away with one click.
//
// Written for the per-button options dialog (button_advanced_panel), which
// grew past the height of a laptop screen: with every section expanded the
// OK / Cancel row was pushed off-screen and the dialog became unusable.
// The dialog now scrolls AND each section collapses, so the user can fold
// away what they are not editing.
//
// The public API is deliberately the subset of QGroupBox the old code used
// (setTitle / setCheckable / setChecked / isChecked / toggled) so switching
// a group over is a type change, not a rewrite. Difference from QGroupBox:
// the content lives in a body widget handed over with setContentLayout(),
// because collapsing = hiding that body.
//
// No animation on purpose (UI rule: no decorative motion) - the fold is
// instant, which also keeps it correct for bodies whose height changes
// with their content (e.g. the stream/playlist reshaping in the button
// dialog).

#pragma once

#include <QWidget>

class QToolButton;
class QCheckBox;
class QFrame;
class QLayout;

class SectionBox : public QWidget {
    Q_OBJECT
public:
    explicit SectionBox(const QString &title = QString(), QWidget *parent = nullptr);

    void    setTitle(const QString &title);
    QString title() const;

    // Optional enable checkbox in the header, same semantics as a checkable
    // QGroupBox: unchecked greys out (but keeps) the contents.
    void setCheckable(bool on);
    bool isCheckable() const { return m_checkable; }
    bool isChecked() const;
    void setChecked(bool on);

    void setContentLayout(QLayout *layout);
    // Parent for widgets that belong to this section. Out-of-line because
    // QFrame is only forward-declared here.
    QWidget *body() const;

    bool isExpanded() const { return m_expanded; }
    void setExpanded(bool on);

    // Remember the fold state across dialog re-opens under
    // "sections/<key>". Empty key = not persisted.
    void setPersistenceKey(const QString &key);

    // Re-apply the theme-derived inline styles (header + body frame).
    void refreshTheme();

signals:
    void toggled(bool checked);      // mirrors QGroupBox::toggled
    void expandedChanged(bool expanded);

private:
    QToolButton *m_header  = nullptr;
    QCheckBox   *m_check   = nullptr;
    QFrame      *m_body    = nullptr;
    QString      m_title;
    QString      m_persistKey;
    bool         m_checkable = false;
    bool         m_expanded  = true;
};
