#include "section_box.h"
#include "theme.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QToolButton>
#include <QCheckBox>
#include <QFrame>
#include <QSettings>

SectionBox::SectionBox(const QString &title, QWidget *parent)
    : QWidget(parent)
    , m_title(title)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto *headerRow = new QWidget(this);
    headerRow->setObjectName(QStringLiteral("sectionHeader"));
    auto *hl = new QHBoxLayout(headerRow);
    hl->setContentsMargins(6, 2, 6, 2);
    hl->setSpacing(6);

    m_check = new QCheckBox(headerRow);
    m_check->setVisible(false);
    hl->addWidget(m_check, 0);

    m_header = new QToolButton(headerRow);
    m_header->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_header->setArrowType(Qt::DownArrow);
    m_header->setText(title);
    m_header->setAutoRaise(true);
    m_header->setCursor(Qt::PointingHandCursor);
    m_header->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_header->setToolTip(tr("Click to fold / unfold this section"));
    hl->addWidget(m_header, 1);

    m_body = new QFrame(this);
    m_body->setObjectName(QStringLiteral("sectionBody"));
    m_body->setFrameShape(QFrame::NoFrame);

    root->addWidget(headerRow);
    root->addWidget(m_body);

    refreshTheme();

    connect(m_header, &QToolButton::clicked, this,
            [this]{ setExpanded(!m_expanded); });
    connect(m_check, &QCheckBox::toggled, this, [this](bool on){
        // Same visual contract as a checkable QGroupBox: the contents stay
        // on screen but go dead so the user sees the values are ignored.
        if (m_body) m_body->setEnabled(on);
        emit toggled(on);
    });
}

void SectionBox::setTitle(const QString &title)
{
    m_title = title;
    if (m_header) m_header->setText(title);
}

QString SectionBox::title() const { return m_title; }

QWidget *SectionBox::body() const { return m_body; }

void SectionBox::setCheckable(bool on)
{
    m_checkable = on;
    if (m_check) m_check->setVisible(on);
    if (m_body && on) m_body->setEnabled(m_check->isChecked());
    else if (m_body)  m_body->setEnabled(true);
}

bool SectionBox::isChecked() const
{
    return m_checkable ? m_check->isChecked() : true;
}

void SectionBox::setChecked(bool on)
{
    if (!m_check) return;
    m_check->setChecked(on);
    if (m_checkable && m_body) m_body->setEnabled(on);
}

void SectionBox::setContentLayout(QLayout *layout)
{
    if (!layout || !m_body) return;
    delete m_body->layout();
    m_body->setLayout(layout);
}

void SectionBox::setExpanded(bool on)
{
    if (m_expanded == on && m_body && m_body->isVisibleTo(this) == on) return;
    m_expanded = on;
    if (m_header) m_header->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
    if (m_body)   m_body->setVisible(on);
    if (!m_persistKey.isEmpty()) {
        QSettings s(QStringLiteral("GameBaiters"), QStringLiteral("Soundboard"));
        s.setValue(QStringLiteral("sections/") + m_persistKey, on);
    }
    emit expandedChanged(on);
}

void SectionBox::setPersistenceKey(const QString &key)
{
    m_persistKey = key;
    if (key.isEmpty()) return;
    QSettings s(QStringLiteral("GameBaiters"), QStringLiteral("Soundboard"));
    const bool saved =
        s.value(QStringLiteral("sections/") + key, m_expanded).toBool();
    if (saved != m_expanded) setExpanded(saved);
}

void SectionBox::refreshTheme()
{
    const Theme::Derived &d = Theme::derivedCached();
    // Object-name scoped so nothing leaks into the section's children (the
    // TS3 host stylesheet is a step away in the cascade).
    setStyleSheet(QString(
        "#sectionHeader { background-color: %1; border: 1px solid %2;"
        " border-top-left-radius: 5px; border-top-right-radius: 5px; }"
        "#sectionBody { background-color: %3; border: 1px solid %2;"
        " border-top: none; border-bottom-left-radius: 5px;"
        " border-bottom-right-radius: 5px; }")
        .arg(d.surfaceAlt.name(), d.border.name(), d.surface.name()));
    if (m_header) {
        m_header->setStyleSheet(QString(
            "QToolButton { border: none; background: transparent;"
            " font-weight: bold; color: %1; padding: 2px; text-align: left; }"
            "QToolButton:hover { color: %2; }")
            .arg(d.text.name(), d.accent.name()));
    }
}
