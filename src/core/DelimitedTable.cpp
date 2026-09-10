#include "core/DelimitedTable.h"

#include <QRegularExpression>

#include <limits>

namespace DelimitedTable {

QChar detectDelimiter(const QString& line)
{
    static const QRegularExpression whitespace(QStringLiteral("\\s+"));

    const int commas = line.count(',');
    const int tabs   = line.count('\t');
    const int semis  = line.count(';');
    const int spaces = line.split(whitespace, Qt::SkipEmptyParts).size() - 1;

    int   best = commas;
    QChar ch   = ',';
    if (tabs  > best) { best = tabs;  ch = '\t'; }
    if (semis > best) { best = semis; ch = ';';  }
    if (spaces > best) { ch = ' '; }
    return ch;
}

QStringList splitLine(const QString& line, QChar delimiter)
{
    static const QRegularExpression whitespace(QStringLiteral("\\s+"));

    if (delimiter == ' ')
        return line.split(whitespace, Qt::SkipEmptyParts);

    QStringList result;
    QString     current;
    bool        inQuotes = false;

    for (int i = 0; i < line.length(); ++i) {
        const QChar c = line[i];
        if (c == '"') {
            inQuotes = !inQuotes;
        } else if (c == delimiter && !inQuotes) {
            result << current.trimmed();
            current.clear();
        } else {
            current += c;
        }
    }
    result << current.trimmed();
    return result;
}

double Csv::number(int row, const QString& name) const
{
    const QString s = value(row, name).trimmed();
    if (s.isEmpty()) return std::numeric_limits<double>::quiet_NaN();
    bool ok = false;
    const double v = s.toDouble(&ok);
    return ok ? v : std::numeric_limits<double>::quiet_NaN();
}

Csv parseCsv(const QByteArray& body)
{
    Csv out;
    const QString text = QString::fromUtf8(body);

    auto splitCommaQuoted = [](const QString& line) {
        QStringList fields;
        QString     cur;
        bool        quoted = false;
        for (const QChar ch : line) {
            if (ch == '"') {
                quoted = !quoted;
            } else if (ch == ',' && !quoted) {
                fields << cur.trimmed();
                cur.clear();
            } else if (ch != '\r') {
                cur += ch;
            }
        }
        fields << cur.trimmed();
        return fields;
    };

    bool haveHeader = false;
    for (const QString& raw : text.split('\n', Qt::SkipEmptyParts)) {
        const QString line = raw.trimmed();
        if (line.isEmpty() || line.startsWith('#'))
            continue;
        const QStringList fields = splitCommaQuoted(line);
        if (!haveHeader) {
            for (int i = 0; i < fields.size(); ++i)
                out.columns.insert(fields.at(i).toLower(), i);
            haveHeader = true;
        } else {
            out.rows.append(fields);
        }
    }
    return out;
}

}   // namespace DelimitedTable
