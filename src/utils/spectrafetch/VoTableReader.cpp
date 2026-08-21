// src/utils/spectrafetch/VoTableReader.cpp

#include "VoTableReader.h"

#include <QXmlStreamReader>

namespace VoTable {

int Table::columnByName(const QString& name) const {
    for (int i = 0; i < fields.size(); ++i)
        if (fields.at(i).name.compare(name, Qt::CaseInsensitive) == 0)
            return i;
    for (int i = 0; i < fields.size(); ++i)
        if (fields.at(i).id.compare(name, Qt::CaseInsensitive) == 0)
            return i;
    return -1;
}

int Table::columnByUcd(const QString& ucdFragment) const {
    for (int i = 0; i < fields.size(); ++i)
        if (fields.at(i).ucd.contains(ucdFragment, Qt::CaseInsensitive))
            return i;
    return -1;
}

Document parse(const QByteArray& xml) {
    Document doc;
    QXmlStreamReader r(xml);

    Table*  table     = nullptr;   // TABLE currently being filled
    bool    inData    = false;     // inside TABLEDATA
    QStringList row;
    QString cell;
    bool    inCell    = false;

    while (!r.atEnd()) {
        const QXmlStreamReader::TokenType tok = r.readNext();

        if (tok == QXmlStreamReader::StartElement) {
            const auto name = r.name();
            if (name.compare(QLatin1String("TABLE"), Qt::CaseInsensitive) == 0) {
                doc.tables.append(Table());
                table = &doc.tables.last();
                table->name = r.attributes().value("name").toString();
            } else if (name.compare(QLatin1String("FIELD"), Qt::CaseInsensitive) == 0) {
                if (table) {
                    Field f;
                    f.id       = r.attributes().value("ID").toString();
                    f.name     = r.attributes().value("name").toString();
                    f.ucd      = r.attributes().value("ucd").toString();
                    f.datatype = r.attributes().value("datatype").toString();
                    table->fields.append(f);
                }
            } else if (name.compare(QLatin1String("TABLEDATA"), Qt::CaseInsensitive) == 0) {
                inData = true;
            } else if (name.compare(QLatin1String("TR"), Qt::CaseInsensitive) == 0) {
                row.clear();
            } else if (name.compare(QLatin1String("TD"), Qt::CaseInsensitive) == 0) {
                cell.clear();
                inCell = true;
            } else if (name.compare(QLatin1String("INFO"), Qt::CaseInsensitive) == 0) {
                // TAP/DALI error reporting: <INFO name="QUERY_STATUS" value="ERROR">msg
                const auto attrs = r.attributes();
                if (attrs.value("name").compare(QLatin1String("QUERY_STATUS"),
                                                Qt::CaseInsensitive) == 0 &&
                    attrs.value("value").compare(QLatin1String("ERROR"),
                                                 Qt::CaseInsensitive) == 0) {
                    const QString msg = r.readElementText(
                        QXmlStreamReader::IncludeChildElements).simplified();
                    doc.error = msg.isEmpty()
                                    ? QStringLiteral("service reported an error")
                                    : msg;
                }
            }
        } else if (tok == QXmlStreamReader::Characters) {
            if (inCell && inData)
                cell += r.text();
        } else if (tok == QXmlStreamReader::EndElement) {
            const auto name = r.name();
            if (name.compare(QLatin1String("TD"), Qt::CaseInsensitive) == 0) {
                if (inData) row.append(cell.trimmed());
                inCell = false;
            } else if (name.compare(QLatin1String("TR"), Qt::CaseInsensitive) == 0) {
                if (inData && table) table->rows.append(row);
            } else if (name.compare(QLatin1String("TABLEDATA"), Qt::CaseInsensitive) == 0) {
                inData = false;
            } else if (name.compare(QLatin1String("TABLE"), Qt::CaseInsensitive) == 0) {
                table = nullptr;
            }
        }
    }

    if (r.hasError() && doc.error.isEmpty())
        doc.error = QStringLiteral("VOTable parse error: %1").arg(r.errorString());

    return doc;
}

}   // namespace VoTable
