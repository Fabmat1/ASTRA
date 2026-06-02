#pragma once

#include <QDoubleSpinBox>
#include <QLineEdit>

class PreciseDoubleSpinBox : public QDoubleSpinBox {
    Q_OBJECT
  public:
    explicit PreciseDoubleSpinBox(QWidget *parent = nullptr)
        : QDoubleSpinBox(parent) {
        setDecimals(15);
        setGroupSeparatorShown(false);
        setKeyboardTracking(false);
        setCorrectionMode(QAbstractSpinBox::CorrectToNearestValue);
        if (lineEdit())
            lineEdit()->setContextMenuPolicy(Qt::DefaultContextMenu);
    }

    QValidator::State validate(QString &text, int & /*pos*/) const override {
        QString t = stripAffixes(text);
        if (t.isEmpty() || t == "-" || t == "+" || t == "." ||
            t.endsWith('e', Qt::CaseInsensitive) ||
            t.endsWith("e-", Qt::CaseInsensitive) ||
            t.endsWith("e+", Qt::CaseInsensitive))
            return QValidator::Intermediate;

        bool ok = false;
        t.toDouble(&ok);
        return ok ? QValidator::Acceptable : QValidator::Intermediate;
    }

    double valueFromText(const QString &text) const override {
        return stripAffixes(text).toDouble();
    }

    QString textFromValue(double value) const override {
        return QString::number(value, 'g', 15);
    }

  private:
    // Remove the prefix/suffix that the base class would normally handle,
    // so our custom parsing sees only the numeric part.
    QString stripAffixes(const QString &in) const {
        QString       t   = in;
        const QString pre = prefix();
        const QString suf = suffix();
        if (!pre.isEmpty() && t.startsWith(pre))
            t.remove(0, pre.size());
        if (!suf.isEmpty() && t.endsWith(suf))
            t.chop(suf.size());
        return t.trimmed();
    }
};