// src/utils/spectrafetch/TapHelpers.cpp

#include "TapHelpers.h"

#include "utils/CdsTapClient.h"

#include <QHttpMultiPart>
#include <QNetworkRequest>
#include <QTextStream>
#include <QUrlQuery>

namespace SpecFetch {

QByteArray buildPositionVOTable(const std::vector<StarQuery>& stars) {
    QString     xml;
    QTextStream ts(&xml);
    ts << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
       << "<VOTABLE version=\"1.3\" "
          "xmlns=\"http://www.ivoa.net/xml/VOTable/v1.3\">\n"
       << " <RESOURCE>\n  <TABLE>\n"
       << "   <FIELD name=\"idx\" datatype=\"int\"/>\n"
       << "   <FIELD name=\"ra\"  datatype=\"double\" unit=\"deg\"/>\n"
       << "   <FIELD name=\"dec\" datatype=\"double\" unit=\"deg\"/>\n"
       << "   <DATA>\n    <TABLEDATA>\n";

    for (int i = 0; i < static_cast<int>(stars.size()); ++i) {
        ts << "     <TR><TD>" << i << "</TD><TD>"
           << QString::number(stars[i].ra, 'f', 8) << "</TD><TD>"
           << QString::number(stars[i].dec, 'f', 8) << "</TD></TR>\n";
    }

    ts << "    </TABLEDATA>\n   </DATA>\n  </TABLE>\n "
          "</RESOURCE>\n</VOTABLE>\n";
    ts.flush();
    return xml.toUtf8();
}

QByteArray tapUploadQuery(QNetworkAccessManager* nam, const QString& url,
                          const QString& adql, const QByteArray& votable,
                          const QString& uploadName, const QString& format,
                          int timeoutMs, QString* error) {
    // Rebuilt per attempt: a QHttpMultiPart is consumed by the reply that
    // sends it and cannot be replayed on a retry.
    auto makeBody = [&adql, &votable, &uploadName, &format]() {
        QHttpMultiPart* multiPart =
            new QHttpMultiPart(QHttpMultiPart::FormDataType);

        auto addField = [multiPart](const QString& name, const QString& value) {
            QHttpPart part;
            part.setHeader(QNetworkRequest::ContentDispositionHeader,
                           QVariant(QString("form-data; name=\"%1\"").arg(name)));
            part.setBody(value.toUtf8());
            multiPart->append(part);
        };

        addField("REQUEST", "doQuery");
        addField("LANG", "ADQL");
        addField("FORMAT", format);
        addField("UPLOAD", uploadName + ",param:upltable");
        addField("QUERY", adql);

        QHttpPart tablePart;
        tablePart.setHeader(QNetworkRequest::ContentTypeHeader,
                            QVariant("application/x-votable+xml"));
        tablePart.setHeader(
            QNetworkRequest::ContentDispositionHeader,
            QVariant("form-data; name=\"upltable\"; filename=\"upltable.xml\""));
        tablePart.setBody(votable);
        multiPart->append(tablePart);
        return multiPart;
    };

    const CdsTap::Response resp =
        CdsTap::postMultipart(nam, url, makeBody, timeoutMs);
    if (!resp.ok()) {
        if (error) *error = resp.error;
        return QByteArray();
    }
    if (error) error->clear();
    return resp.body;
}

QByteArray tapQuery(QNetworkAccessManager* nam, const QString& url,
                    const QString& adql, const QString& format, int timeoutMs,
                    QString* error) {
    QUrlQuery form;
    form.addQueryItem("REQUEST", "doQuery");
    form.addQueryItem("LANG", "ADQL");
    form.addQueryItem("FORMAT", format);
    form.addQueryItem("QUERY", adql);

    const CdsTap::Response resp = CdsTap::postForm(nam, url, form, timeoutMs);
    if (!resp.ok()) {
        if (error) *error = resp.error;
        return QByteArray();
    }
    if (error) error->clear();
    return resp.body;
}

Csv parseCsv(const QByteArray& body) {
    Csv out;

    // Split into logical lines, honoring quoted fields (a quoted field may
    // contain commas; embedded newlines are not expected in TAP output).
    const QString text = QString::fromUtf8(body);
    const QStringList lines =
        text.split('\n', Qt::SkipEmptyParts);

    auto splitLine = [](const QString& line) {
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
    for (const QString& raw : lines) {
        const QString line = raw.trimmed();
        if (line.isEmpty() || line.startsWith('#'))
            continue;
        const QStringList fields = splitLine(line);
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

}   // namespace SpecFetch
