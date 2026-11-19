#pragma once

// The sky map's layer picker: a scrolling column of cards, one per image
// source, each with a small preview of that source around the star.
//
// HiPS survey cards render their own preview from the shared tile loader (at
// a field wide enough to show a few of the survey's pixels) and find out on
// the way whether the survey covers the star; cards for surveys that do not,
// or whose servers cannot be reached, are greyed. Cards for other layers (the
// lightcurvequery previews) are filled in by the owner.

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
        SubtitleRole,
        StatusRole,
        StateRole,
        ThumbRole,
        HeaderRole,
    };

    static constexpr int kThumbPx = 64;   // logical size of a card's preview

    explicit SkyLayerList(HipsTileLoader* loader, QWidget* parent = nullptr);

    void setTarget(double raDeg, double decDeg);

    void addHeader(const QString& title);
    void addLayer(const QString& key, const QString& title, const QString& subtitle,
                  const QString& tooltip = QString());
    /// Append a header per regime and a card per survey of hipsSurveys().
    void addHipsSurveys();

    /// Preview and state of a card the owner manages.
    void setLayerPreview(const QString& key, const QImage& thumb, State state,
                         const QString& status);
    void setLayerTitle(const QString& key, const QString& title);
    State layerState(const QString& key) const;

    /// Begin resolving the surveys and rendering their previews. Nothing goes
    /// on the network before this.
    void startPreviews();

    QString currentKey() const;
    bool    setCurrentKey(const QString& key);
    /// Move to the next (+1) or previous (-1) card, skipping greyed ones.
    void    step(int delta);

    QSize sizeHint() const override { return { 280, 600 }; }

signals:
    void currentKeyChanged(const QString& key);

private:
    QStandardItem* itemFor(const QString& key) const;
    void schedulePreview(const QString& surveyId);
    void renderPreview(const QString& surveyId);
    void setState(QStandardItem* it, State state, const QString& status);

    HipsTileLoader*                _loader = nullptr;
    QStandardItemModel*            _model  = nullptr;
    QHash<QString, QStandardItem*> _items;
    QSet<QString>                  _dirty;      // surveys whose preview needs redrawing
    QSet<QString>                  _final;      // previews complete, nothing more will arrive
    QTimer*                        _timer  = nullptr;
    bool                           _started = false;
    double _ra  = std::numeric_limits<double>::quiet_NaN();
    double _dec = std::numeric_limits<double>::quiet_NaN();
};

} // namespace SkyMap
