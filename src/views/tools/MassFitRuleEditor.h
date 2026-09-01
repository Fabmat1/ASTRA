#pragma once

#include <QDialog>
#include <QVector>

#include "models/MassFitPlan.h"

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QSpinBox;
class QVBoxLayout;
class QWidget;

// ─────────────────────────────────────────────────────────────────────────────
// Editor for one RuleGroup: an All/Any selector over a list of conditions on
// what a completed fit attempt produced.
//
// Deliberately one level deep. RuleGroup can nest, but a tree of AND/OR boxes
// inside a modal dialog is a lot of interface for a question that in practice
// reads "teff > 30000 AND logg < 5.2". A group loaded from JSON that does
// carry sub-groups keeps them: they are passed through untouched and the
// dialog says so rather than quietly dropping them.
// ─────────────────────────────────────────────────────────────────────────────
class MassFitRuleEditor : public QDialog
{
    Q_OBJECT
public:
    MassFitRuleEditor(const astra::massfit::RuleGroup& rule,
                      const QString& title,
                      QWidget* parent = nullptr);

    astra::massfit::RuleGroup rule() const;

private:
    /// One condition row's widgets. `host` owns them all, so removing a row is
    /// a single deleteLater on it.
    struct Row {
        QWidget*        host      = nullptr;
        QComboBox*      field     = nullptr;
        QComboBox*      op        = nullptr;
        QDoubleSpinBox* lo        = nullptr;
        QDoubleSpinBox* hi        = nullptr;
        QLabel*         andLabel  = nullptr;
        QSpinBox*       component = nullptr;
        QLabel*         compLabel = nullptr;
    };

    void addRow(const astra::massfit::Condition& c);
    void syncRowVisibility(const Row& r);
    void updatePreview();

    astra::massfit::RuleGroup _rule;    ///< holds the nested groups verbatim

    QComboBox*   _combineCombo = nullptr;
    QVBoxLayout* _rowsLayout   = nullptr;
    QLabel*      _preview      = nullptr;
    QLabel*      _nestedNote   = nullptr;

    QVector<Row> _rows;
};
