#include "PlatformStyle.h"

#ifdef __APPLE__

#include <QWidget>
#include <QStyle>
#include <QStyleFactory>
#include <QEvent>
#include <QChildEvent>
#include <QPointer>

namespace {

// One shared Fusion instance for every soundboard widget. QWidget::
// setStyle does not take ownership, and QStyle instances are designed
// to be shared across widgets; parenting it to nothing and leaking it
// for the plugin lifetime is intentional (deleting it while widgets
// still reference it on plugin teardown would be the real bug — Qt
// resets widgets to the app style when they are destroyed anyway).
QStyle *fusionStyle()
{
    static QStyle *s = QStyleFactory::create(QStringLiteral("Fusion"));
    return s;
}

void styleOne(QWidget *w)
{
    QStyle *f = fusionStyle();
    if (!f) return;
    if (w->style() != f) w->setStyle(f);
    // The aqua focus ring paints OVER stylesheet borders and makes
    // every focused input look broken against the dark theme.
    w->setAttribute(Qt::WA_MacShowFocusRect, false);
}

// Recursive filter: styles a widget subtree and installs itself on
// every widget in it, so widgets created later anywhere inside the
// tree get the Fusion style the moment they are added.
class FusionPropagator : public QObject {
public:
    static FusionPropagator *instance()
    {
        static FusionPropagator *p = new FusionPropagator();
        return p;
    }

    void cover(QWidget *w)
    {
        if (!w) return;
        styleOne(w);
        w->removeEventFilter(this);   // idempotent re-apply
        w->installEventFilter(this);
        const auto children = w->findChildren<QWidget *>();
        for (QWidget *c : children) {
            styleOne(c);
            c->removeEventFilter(this);
            c->installEventFilter(this);
        }
    }

protected:
    bool eventFilter(QObject *obj, QEvent *ev) override
    {
        if (ev->type() == QEvent::ChildAdded) {
            QObject *child = static_cast<QChildEvent *>(ev)->child();
            if (child && child->isWidgetType()) {
                // The child is mid-construction here; setting the style
                // is safe now, and covering it recursively hooks the
                // grandchildren it will create during its constructor.
                cover(static_cast<QWidget *>(child));
            }
        }
        return QObject::eventFilter(obj, ev);
    }
};

} // namespace

namespace PlatformStyle {

void apply(QWidget *root)
{
    if (!root) return;
    FusionPropagator::instance()->cover(root);
}

}

#else // !__APPLE__

namespace PlatformStyle {
void apply(QWidget *) {}
}

#endif
