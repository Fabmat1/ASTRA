#pragma once

#include <QFrame>

class QButtonGroup;

namespace astra {

/// A row of joined, mutually exclusive buttons in one rounded frame: the
/// either-or switch (Normalized | Flux | Raw). The checked segment is filled
/// with the theme's accent, the others stay flat, so it reads as a switch
/// rather than as separate buttons. Restyles itself when the theme changes.
class SegmentedControl : public QFrame
{
    Q_OBJECT
public:
    explicit SegmentedControl(QWidget* parent = nullptr);

    /// Append a segment; its id in group() is its position (0, 1, ...).
    int addSegment(const QString& text, const QString& toolTip = {});
    QButtonGroup* group() const { return _group; }

protected:
    void changeEvent(QEvent* ev) override;

private:
    void restyle();

    QButtonGroup* _group = nullptr;
    QString       _sheet;      ///< last sheet set, to stop StyleChange loops
};

}   // namespace astra
