#include "FilterExpression.h"
#include "models/ColumnPreset.h"

#include <QRegularExpression>
#include <QStringList>
#include <cmath>
#include <limits>
#include <map>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// AST node
// ─────────────────────────────────────────────────────────────────────────────
struct FilterExpression::Node
{
    enum Type { Number, Column, Negate, Binary, Compare, Func } type = Number;

    double num = 0.0;
    QString name;                       // column key, operator symbol
    double (*fn)(double) = nullptr;     // Func only
    std::shared_ptr<const Node> a, b;

    double eval(const FilterExpression::ColumnResolver& resolver) const;
};

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// ── Column name resolution ───────────────────────────────────────────────────
// Accepts an internal key or a display name (case-insensitive); returns the
// canonical key, or an empty string if unknown.
QString resolveColumnKey(const QString& raw)
{
    const QString name = raw.trimmed();
    if (name.isEmpty()) return QString();

    auto& mgr = ColumnPresetManager::instance();
    if (mgr.columnDef(name)) return name;

    for (const auto& c : mgr.allColumns()) {
        if (QString::compare(c.displayName, name, Qt::CaseInsensitive) == 0)
            return c.key;
    }
    for (const auto& c : mgr.allColumns()) {
        if (QString::compare(c.key, name, Qt::CaseInsensitive) == 0)
            return c.key;
    }
    return QString();
}

// ── Functions ────────────────────────────────────────────────────────────────
double (*lookupFunction(const QString& name))(double)
{
    static const std::map<QString, double (*)(double)> fns = {
        {"abs",   [](double x) { return std::fabs(x); }},
        {"sqrt",  [](double x) { return std::sqrt(x); }},
        {"cbrt",  [](double x) { return std::cbrt(x); }},
        {"log",   [](double x) { return std::log(x); }},
        {"log10", [](double x) { return std::log10(x); }},
        {"exp",   [](double x) { return std::exp(x); }},
        {"floor", [](double x) { return std::floor(x); }},
        {"ceil",  [](double x) { return std::ceil(x); }},
        {"round", [](double x) { return std::round(x); }},
        {"sin",   [](double x) { return std::sin(x); }},
        {"cos",   [](double x) { return std::cos(x); }},
        {"tan",   [](double x) { return std::tan(x); }},
    };
    auto it = fns.find(name);
    return it == fns.end() ? nullptr : it->second;
}

// ── Tokenizer ────────────────────────────────────────────────────────────────
enum class TokType { Number, Column, Func, Sym, End };

struct Token
{
    TokType type = TokType::End;
    QString text;       // symbol, function name, or resolved column key
    double num = 0.0;
};

bool tokenize(const QString& text, std::vector<Token>& out, QString* err)
{
    static const QRegularExpression numRe(
        QStringLiteral("\\d*\\.?\\d+([eE][+-]?\\d+)?"));
    static const QStringList compTwoChar = {">=", "<=", "==", "!=", "<>"};
    static const QString singleSyms = QStringLiteral("+-*/^()<>=");

    const int n = text.size();
    int i = 0;
    while (i < n) {
        const QChar c = text[i];
        if (c.isSpace()) { ++i; continue; }

        // {Display Name} or {key}
        if (c == '{') {
            const int close = text.indexOf('}', i + 1);
            if (close < 0) { *err = QStringLiteral("Unterminated '{'"); return false; }
            const QString raw = text.mid(i + 1, close - i - 1);
            const QString key = resolveColumnKey(raw);
            if (key.isEmpty()) {
                *err = QStringLiteral("Unknown column '%1'").arg(raw.trimmed());
                return false;
            }
            out.push_back({TokType::Column, key, 0.0});
            i = close + 1;
            continue;
        }

        // Number literal
        if (c.isDigit() || (c == '.' && i + 1 < n && text[i + 1].isDigit())) {
            const auto m = numRe.match(text, i, QRegularExpression::NormalMatch,
                                       QRegularExpression::AnchorAtOffsetMatchOption);
            Token t;
            t.type = TokType::Number;
            t.num = m.captured(0).toDouble();
            out.push_back(t);
            i += m.capturedLength(0);
            continue;
        }

        // Identifier: function name or bare column reference
        if (c.isLetter() || c == '_') {
            int j = i + 1;
            while (j < n && (text[j].isLetterOrNumber() || text[j] == '_')) ++j;
            const QString word = text.mid(i, j - i);

            int k = j;
            while (k < n && text[k].isSpace()) ++k;
            const bool callSite = (k < n && text[k] == '(');

            if (callSite && lookupFunction(word.toLower())) {
                out.push_back({TokType::Func, word.toLower(), 0.0});
            } else {
                const QString key = resolveColumnKey(word);
                if (key.isEmpty()) {
                    *err = QStringLiteral("Unknown column '%1'").arg(word);
                    return false;
                }
                out.push_back({TokType::Column, key, 0.0});
            }
            i = j;
            continue;
        }

        // Two-character comparison operators
        if (i + 1 < n) {
            const QString two = text.mid(i, 2);
            if (compTwoChar.contains(two)) {
                out.push_back({TokType::Sym, two == "<>" ? QStringLiteral("!=") : two, 0.0});
                i += 2;
                continue;
            }
        }

        // Single-character symbols ('=' treated as '==')
        if (singleSyms.contains(c)) {
            QString s(c);
            if (s == "=") s = QStringLiteral("==");
            out.push_back({TokType::Sym, s, 0.0});
            ++i;
            continue;
        }

        *err = QStringLiteral("Unexpected character '%1'").arg(c);
        return false;
    }
    out.push_back({TokType::End, QString(), 0.0});
    return true;
}

// ── Recursive-descent parser ─────────────────────────────────────────────────
using NodePtr = std::shared_ptr<const FilterExpression::Node>;
using Node = FilterExpression::Node;

struct Parser
{
    const std::vector<Token>& toks;
    size_t pos = 0;
    QString err;

    const Token& peek() const { return toks[pos]; }

    bool isSym(const QString& s) const
    {
        return peek().type == TokType::Sym && peek().text == s;
    }

    bool takeSym(const QString& s)
    {
        if (isSym(s)) { ++pos; return true; }
        return false;
    }

    bool isCompareOp() const
    {
        static const QStringList ops = {">", ">=", "<", "<=", "==", "!="};
        return peek().type == TokType::Sym && ops.contains(peek().text);
    }

    NodePtr make(Node&& n) { return std::make_shared<Node>(std::move(n)); }

    NodePtr parseComparison()
    {
        NodePtr l = parseExpr();
        if (!l) return nullptr;
        if (!isCompareOp()) return l;

        Node cmp;
        cmp.type = Node::Compare;
        cmp.name = peek().text;
        ++pos;
        cmp.a = l;
        cmp.b = parseExpr();
        if (!cmp.b) return nullptr;
        if (isCompareOp()) {
            err = QStringLiteral("Only one comparison is allowed");
            return nullptr;
        }
        return make(std::move(cmp));
    }

    NodePtr parseExpr()
    {
        NodePtr l = parseTerm();
        if (!l) return nullptr;
        while (isSym("+") || isSym("-")) {
            Node bin;
            bin.type = Node::Binary;
            bin.name = peek().text;
            ++pos;
            bin.a = l;
            bin.b = parseTerm();
            if (!bin.b) return nullptr;
            l = make(std::move(bin));
        }
        return l;
    }

    NodePtr parseTerm()
    {
        NodePtr l = parseFactor();
        if (!l) return nullptr;
        while (isSym("*") || isSym("/")) {
            Node bin;
            bin.type = Node::Binary;
            bin.name = peek().text;
            ++pos;
            bin.a = l;
            bin.b = parseFactor();
            if (!bin.b) return nullptr;
            l = make(std::move(bin));
        }
        return l;
    }

    NodePtr parseFactor()
    {
        if (takeSym("-")) {
            Node neg;
            neg.type = Node::Negate;
            neg.a = parseFactor();
            if (!neg.a) return nullptr;
            return make(std::move(neg));
        }
        if (takeSym("+")) return parseFactor();

        NodePtr p = parsePrimary();
        if (!p) return nullptr;
        if (takeSym("^")) {
            Node pw;
            pw.type = Node::Binary;
            pw.name = QStringLiteral("^");
            pw.a = p;
            pw.b = parseFactor();    // right-associative
            if (!pw.b) return nullptr;
            return make(std::move(pw));
        }
        return p;
    }

    NodePtr parsePrimary()
    {
        const Token& t = peek();
        switch (t.type) {
        case TokType::Number: {
            Node n;
            n.type = Node::Number;
            n.num = t.num;
            ++pos;
            return make(std::move(n));
        }
        case TokType::Column: {
            Node n;
            n.type = Node::Column;
            n.name = t.text;
            ++pos;
            return make(std::move(n));
        }
        case TokType::Func: {
            Node n;
            n.type = Node::Func;
            n.name = t.text;
            n.fn = lookupFunction(t.text);
            ++pos;
            if (!takeSym("(")) {
                err = QStringLiteral("Expected '(' after '%1'").arg(n.name);
                return nullptr;
            }
            n.a = parseExpr();
            if (!n.a) return nullptr;
            if (!takeSym(")")) {
                err = QStringLiteral("Expected ')'");
                return nullptr;
            }
            return make(std::move(n));
        }
        case TokType::Sym:
            if (t.text == "(") {
                ++pos;
                NodePtr inner = parseExpr();
                if (!inner) return nullptr;
                if (!takeSym(")")) {
                    err = QStringLiteral("Expected ')'");
                    return nullptr;
                }
                return inner;
            }
            err = QStringLiteral("Unexpected '%1'").arg(t.text);
            return nullptr;
        case TokType::End:
            err = QStringLiteral("Unexpected end of expression");
            return nullptr;
        }
        return nullptr;
    }
};

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Evaluation
// ─────────────────────────────────────────────────────────────────────────────
double FilterExpression::Node::eval(const FilterExpression::ColumnResolver& resolver) const
{
    switch (type) {
    case Number:
        return num;
    case Column: {
        if (!resolver) return kNaN;
        const QVariant v = resolver(name);
        bool ok = false;
        double d = v.toDouble(&ok);
        if (!ok) d = v.toString().toDouble(&ok);
        return ok ? d : kNaN;
    }
    case Negate:
        return -a->eval(resolver);
    case Binary: {
        const double x = a->eval(resolver);
        const double y = b->eval(resolver);
        if (name == "+") return x + y;
        if (name == "-") return x - y;
        if (name == "*") return x * y;
        if (name == "/") return x / y;
        if (name == "^") return std::pow(x, y);
        return kNaN;
    }
    case Func:
        return fn ? fn(a->eval(resolver)) : kNaN;
    case Compare: {
        const double x = a->eval(resolver);
        const double y = b->eval(resolver);
        if (std::isnan(x) || std::isnan(y)) return 0.0;   // missing data → no match
        bool res = false;
        if      (name == ">")  res = x > y;
        else if (name == ">=") res = x >= y;
        else if (name == "<")  res = x < y;
        else if (name == "<=") res = x <= y;
        else if (name == "==") res = x == y;
        else if (name == "!=") res = x != y;
        return res ? 1.0 : 0.0;
    }
    }
    return kNaN;
}

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────
std::shared_ptr<const FilterExpression> FilterExpression::compile(const QString& text,
                                                                  QString* errorOut)
{
    if (errorOut) errorOut->clear();

    std::vector<Token> toks;
    QString err;
    if (!tokenize(text, toks, &err)) {
        if (errorOut) *errorOut = err;
        return nullptr;
    }
    if (toks.size() <= 1) {
        if (errorOut) *errorOut = QStringLiteral("Empty expression");
        return nullptr;
    }

    Parser p{toks};
    NodePtr root = p.parseComparison();
    if (root && p.peek().type != TokType::End) {
        p.err = QStringLiteral("Unexpected '%1'").arg(p.peek().text);
        root = nullptr;
    }
    if (!root) {
        if (errorOut)
            *errorOut = p.err.isEmpty() ? QStringLiteral("Invalid expression") : p.err;
        return nullptr;
    }

    auto expr = std::shared_ptr<FilterExpression>(new FilterExpression());
    expr->_root = root;
    expr->_hasComparison = (root->type == Node::Compare);
    return expr;
}

double FilterExpression::evaluateNumeric(const ColumnResolver& resolver, bool* ok) const
{
    if (_hasComparison || !_root) {
        if (ok) *ok = false;
        return kNaN;
    }
    const double v = _root->eval(resolver);
    if (ok) *ok = !std::isnan(v);
    return v;
}

bool FilterExpression::evaluateBool(const ColumnResolver& resolver, bool* ok) const
{
    if (!_hasComparison || !_root) {
        if (ok) *ok = false;
        return false;
    }
    if (ok) *ok = true;
    return _root->eval(resolver) != 0.0;
}
