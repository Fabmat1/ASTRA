#pragma once

#include <QDoubleSpinBox>
#include <QLineEdit>

// A QDoubleSpinBox that:
//   • keeps up to 15 significant digits (periods!)
//   • accepts pasting of arbitrary numeric text (incl. sci-notation,
//   whitespace) • never strips precision on commit
//
// Reuse this everywhere a high-precision numeric field is needed
// (RV-MCMC dialog, lightcurve fit page, etc.).
class PreciseDoubleSpinBox : public QDoubleSpinBox {
    Q_OBJECT
  public:
    explicit PreciseDoubleSpinBox(QWidget *parent = nullptr)
        : QDoubleSpinBox(parent) {
        setDecimals(15);
        setGroupSeparatorShown(false); // commas/spaces break paste + parsing
        setKeyboardTracking(false);    // commit only when editing finishes
        setCorrectionMode(QAbstractSpinBox::CorrectToNearestValue);
        if (lineEdit())
            lineEdit()->setContextMenuPolicy(
                Qt::DefaultContextMenu); // ensure Paste menu
    }

    // Permit partial input while typing AND full-precision pastes.
    QValidator::State validate(QString &text, int & /*pos*/) const override {
        QString t = text.trimmed();
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
        return text.trimmed().toDouble();
    }

    QString textFromValue(double value) const override {
        // 'g' with 15 sig-figs keeps periods exact without trailing-zero noise.
        return QString::number(value, 'g', 15);
    }
};