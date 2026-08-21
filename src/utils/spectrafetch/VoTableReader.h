// src/utils/spectrafetch/VoTableReader.h
//
// Minimal VOTable (1.1-1.4) TABLEDATA parser for SSAP / DataLink / TAP
// responses. Handles multiple RESOURCE/TABLE elements; BINARY/BINARY2/FITS
// serializations are not supported (every service we query answers TABLEDATA
// when asked for FORMAT=votable).

#ifndef VOTABLEREADER_H
#define VOTABLEREADER_H

#include <QList>
#include <QString>
#include <QStringList>

namespace VoTable {

struct Field {
    QString id;         // FIELD ID attribute
    QString name;       // FIELD name attribute
    QString ucd;
    QString datatype;
};

struct Table {
    QString name;                    // TABLE name attribute (may be empty)
    QList<Field> fields;
    QList<QStringList> rows;         // cell text, one QStringList per TR

    // Case-insensitive lookup by FIELD name, then ID, then UCD substring.
    // Returns -1 when absent.
    int columnByName(const QString& name) const;
    int columnByUcd(const QString& ucdFragment) const;

    QString value(int row, int col) const {
        if (row < 0 || row >= rows.size() || col < 0) return QString();
        const QStringList& r = rows.at(row);
        return col < r.size() ? r.at(col) : QString();
    }
};

struct Document {
    QList<Table> tables;
    QString error;           // parse error, or QUERY_STATUS=ERROR message

    bool ok() const { return error.isEmpty(); }
    const Table* firstTable() const {
        return tables.isEmpty() ? nullptr : &tables.first();
    }
};

Document parse(const QByteArray& xml);

}   // namespace VoTable

#endif   // VOTABLEREADER_H
