#include "remote/ShellQuote.h"

namespace astra::remote {

bool isPlainArg(const QString& arg)
{
    if (arg.isEmpty()) return false;
    for (const QChar c : arg) {
        if (c.isLetterOrNumber() && c.unicode() < 128) continue;
        switch (c.unicode()) {
            case u'_': case u'-': case u'+': case u'=': case u'/':
            case u'.': case u',': case u':': case u'@': case u'%':
            case u'^':
                continue;
            default:
                return false;
        }
    }
    return true;
}

QString shQuote(const QString& arg)
{
    if (isPlainArg(arg)) return arg;
    QString out = arg;
    out.replace(QLatin1Char('\''), QLatin1String("'\\''"));
    return QLatin1Char('\'') + out + QLatin1Char('\'');
}

QString shJoin(const QStringList& args)
{
    QStringList quoted;
    quoted.reserve(args.size());
    for (const QString& a : args) quoted << shQuote(a);
    return quoted.join(QLatin1Char(' '));
}

} // namespace astra::remote
