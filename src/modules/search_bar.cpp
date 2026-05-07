#include "search_bar.h"
#include "help_bubble.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>

SearchBar::SearchBar(QWidget *parent)
    : QWidget(parent)
    , m_edit(new QLineEdit(this))
{
    m_edit->setPlaceholderText(tr("Search buttons..."));
    m_edit->setClearButtonEnabled(true);
    m_edit->setMinimumHeight(28);

    auto *cap = new QLabel(QString::fromUtf8("\xF0\x9F\x94\x8D ") + tr("Search"), this);
    cap->setMinimumWidth(64);

    auto *h = new QHBoxLayout(this);
    h->setContentsMargins(8, 4, 8, 4);
    h->setSpacing(8);
    h->addWidget(cap);
    h->addWidget(m_edit, 1);
    h->addWidget(new HelpBubble(tr(
        "Type to filter the buttons by name. The grid hides any button\n"
        "whose label does not contain what you type. Clear the field\n"
        "(or click the X) to show every button again."), this));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    connect(m_edit, &QLineEdit::textChanged, this, &SearchBar::filterChanged);
}

QString SearchBar::filter() const  { return m_edit->text(); }
void    SearchBar::setFilter(const QString &t) { m_edit->setText(t); }
void    SearchBar::clear()         { m_edit->clear(); }
