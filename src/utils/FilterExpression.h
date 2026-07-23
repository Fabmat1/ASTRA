#ifndef FILTEREXPRESSION_H
#define FILTEREXPRESSION_H

#include <QString>
#include <QVariant>
#include <functional>
#include <memory>

// ─────────────────────────────────────────────────────────────────────────────
// Parsed arithmetic / comparison expression over star columns.
//
// Syntax:
//   - numbers:            5, 0.3, 1e-3
//   - column references:  internal key (plx, e_plx, rv_med), a single-word
//                         display name (Teff), or any display name / key in
//                         braces: {Parallax}, {ΔRV max}
//   - arithmetic:         + - * / ^ and parentheses
//   - functions:          abs sqrt cbrt log log10 exp floor ceil round
//                         sin cos tan
//   - at most one comparison at the top level: > >= < <= == != (= and <> ok)
//
// Rows with missing values (NaN) make comparisons evaluate to false.
// ─────────────────────────────────────────────────────────────────────────────
class FilterExpression
{
public:
    // Maps a canonical column key to the row's value for that column.
    using ColumnResolver = std::function<QVariant(const QString& columnKey)>;

    // Returns nullptr on failure and sets *errorOut to a human-readable reason.
    static std::shared_ptr<const FilterExpression> compile(const QString& text,
                                                           QString* errorOut = nullptr);

    // True if the expression contains a top-level comparison operator.
    bool hasComparison() const { return _hasComparison; }

    // Numeric value of a pure arithmetic expression.
    // *ok is false if the result is NaN (e.g. a referenced column is empty).
    double evaluateNumeric(const ColumnResolver& resolver, bool* ok = nullptr) const;

    // Boolean result of a comparison expression.
    // *ok is false if the expression has no comparison.
    bool evaluateBool(const ColumnResolver& resolver, bool* ok = nullptr) const;

    struct Node;

private:
    FilterExpression() = default;

    std::shared_ptr<const Node> _root;
    bool _hasComparison = false;
};

#endif // FILTEREXPRESSION_H
