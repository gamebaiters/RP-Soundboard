#include "style_helper.h"
#include "modules/theme.h"
#include <QFile>
#include <QTextStream>
#include <QDebug>

QString StyleHelper::loadDarkStyle()
{
    QFile f(":/style/dark_style.qss");
    if (!f.exists()) {
        qWarning() << "Unable to load dark style sheet, file not found";
        return QString();
    }
    if (!f.open(QFile::ReadOnly | QFile::Text)) {
        qWarning() << "Unable to open dark style sheet";
        return QString();
    }
    QTextStream ts(&f);
    QString css = ts.readAll();

    Theme::Colors c = Theme::colors();
    if (!c.enabled) return css;

    // Full retint: every hex literal in dark_style.qss is mapped to a
    // derived theme token so picking a different background recolors all
    // surfaces, borders, hovers, disabled states + auto-contrasts the
    // text. Tokens are listed in the same order they appear in the QSS
    // so it's easy to confirm coverage.
    Theme::Derived d = Theme::derive(c);
    struct Sub { const char *from; QString to; };
    Sub subs[] = {
        // Greys (driven by background).
        { "#2b2b2b", d.bg.name()             },
        { "#1e1e1e", d.surfaceAlt.name()     },
        { "#3a3a3a", d.button.name()         },
        { "#4a4a4a", d.hover.name()          },
        { "#1f1f1f", d.pressed.name()        },
        { "#5a5a5a", d.selectionBg.name()    },
        { "#555555", d.border.name()         },
        { "#777777", d.borderStrong.name()   },
        { "#888888", d.slider.name()         },
        { "#dcdcdc", d.text.name()           },
        { "#cccccc", d.text.name()           },
        { "#6a6a6a", d.textMuted.name()      },
        { "#161616", d.disabledBg.name()     },
        { "#2a2a2a", d.disabledSurface.name()},
        { "#2e2e2e", d.disabledBorder.name() },
        { "#252525", d.altRow.name()         },
        // Accent variants.
        { "#3a5a7a", d.accentDisabled.name() },
        { "#3a78c2", d.accentDark.name()     },
        { "#6ab0ff", d.accentLight.name()    },
        { "#4a90e2", d.accent.name()         },
    };
    for (const auto &s : subs) css.replace(QLatin1String(s.from), s.to);
    return css;
}
