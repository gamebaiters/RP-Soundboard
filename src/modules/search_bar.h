// SearchBar - top-pinned filter input. Real-time filter on every keystroke.
// Pure UI; emits filterChanged signal. Replaces the old filterEdit.

#pragma once

#include <QWidget>

class QLineEdit;

class SearchBar : public QWidget {
    Q_OBJECT
public:
    explicit SearchBar(QWidget *parent = nullptr);

    QString filter() const;
    void    setFilter(const QString &text);
    void    clear();

signals:
    void filterChanged(const QString &text);

private:
    QLineEdit *m_edit;
};
