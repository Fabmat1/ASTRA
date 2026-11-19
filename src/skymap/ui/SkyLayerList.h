#pragma once

// The sky map's layer picker: a scrolling column of slim cards, one per image
// source, each showing that source around the star as its background.
//
// HiPS survey cards render their own preview strip from the shared tile
// loader (at a field wide enough to show a few of the survey's pixels) and
// find out on the way whether the survey covers the star. Cards for surveys
// that do not, or whose servers cannot be reached, are collapsed behind one
// "N more" row at the end of the list, which shows them (greyed) on demand.
// Cards for other layers (the lightcurvequery previews) are filled in by the
// owner.

#include <QHash>
#include <QImage>
#include <QListView>
#include <QSet>
#include <QString>

#include <limits>

class QStandardItem;
class QStandardItemModel;
class QTimer;

namespace SkyMap {

class HipsTileLoader;

/// Key prefix of HiPS survey cards: "hips:" + survey id.
inline const QString kHipsLayerPrefix = QStringLiteral("hips:");

class SkyLayerList : public QListView
{
    Q_OBJECT
public:
    enum class State { Pending, Ready, NoCoverage, Unavailable };

    // Item data roles, for the delegate.
    enum Role {
        KeyRole = Qt::UserRole + 1,
        StatusRole,
        StateRole,
        ThumbRole,
        HeaderRole,
        FooterRole,
    };

    // Logical size previews are rendered at; cards crop them to their own
    // shape, centred on the star.
    static constexpr int kPreviewWidth  = 300;
    static constexpr int kPreviewHeight = 56;

    explicit SkyLayerList(HipsTileLoader* loader, QWidget* parent = nullptr);

    void setTarget(double raDeg, double decDeg);

    void addHeader(const QString& title);
    void addLayer(const QString& key, const QString& title, const QString& tooltip = QString());
    /// Append a header per regime and a card per survey of hipsSurveys().
    void addHipsSurveys();

    /// Preview and state of a card the owner manages.
    void setLayerPreview(const QString& key, const QImage& preview, State state,
                         const QString& status);
    State layerState(const QString& key) const;

    /// Begin resolving the surveys and rendering their previews. Nothing goes
    /// on the network before this.
    void startPreviews();

    QString currentKey() const;
    bool    setCurrentKey(const QString& key);
    /// Move to the next (+1) or previous (-1) card that has coverage.
    void    step(int delta);

    QSize sizeHint() const override { return { 260, 600 }; }

signals:
    void currentKeyChanged(const QString& key);

private:
    QStandardItem* itemFor(const QString& key) const;
    void appendRow(QStandardItem* it);   // keeps the footer last
    void schedulePreview(const QString& surveyId);
    void renderPreview(const QString& surveyId);
    void setState(QStandardItem* it, State state, const QString& status);
    /// Hide cards without coverage (unless shown on demand) and headers left
    /// without cards; update the footer's count.
    void updateVisibility();

    HipsTileLoader*                _loader = nullptr;
    QStandardItemModel*            _model  = nullptr;
    QStandardItem*                 _footer = nullptr;
    QHash<QString, QStandardItem*> _items;
    QSet<QString>                  _dirty;      // surveys whose preview needs redrawing
    QSet<QString>                  _final;      // previews complete, nothing more will arrive
    QTimer*                        _timer  = nullptr;
    QTimer*                        _visibilityTimer = nullptr;
    bool                           _started = false;
    bool                           _showUncovered = false;
    double _ra  = std::numeric_limits<double>::quiet_NaN();
    double _dec = std::numeric_limits<double>::quiet_NaN();
};

} // namespace SkyMap
