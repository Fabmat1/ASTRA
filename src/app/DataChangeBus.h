#pragma once

/*  App-wide notice board for data that changed behind the window's back.
 *
 *  The database layer emits nothing, so a view only learns about a change if
 *  whoever made it tells it. Writers that are not the view itself (the AI
 *  tools, a background service) post here after they have written; the star
 *  table and open detail windows listen and redraw.
 *
 *  Signals are emitted on the main thread; post from elsewhere and they are
 *  queued there.                                                            */

#include <QObject>
#include <QString>
#include <QStringList>

class DataChangeBus : public QObject {
    Q_OBJECT
public:
    static DataChangeBus& instance();

    /// Fields of an existing star changed (empty list = unknown which).
    void postStarChanged(const QString& projectId, const QString& starId,
                         const QStringList& fields = {});
    void postStarsAdded(const QString& projectId, const QStringList& starIds);
    void postStarsRemoved(const QString& projectId, const QStringList& starIds);
    /// Child data of a star changed. kind: "spectra", "fits", "rv",
    /// "lightcurves", "photometry", "sed", "periodograms".
    void postStarDataChanged(const QString& projectId, const QString& starId,
                             const QString& kind);
    void postProjectsChanged();

signals:
    void starChanged(const QString& projectId, const QString& starId, const QStringList& fields);
    void starsAdded(const QString& projectId, const QStringList& starIds);
    void starsRemoved(const QString& projectId, const QStringList& starIds);
    void starDataChanged(const QString& projectId, const QString& starId, const QString& kind);
    void projectsChanged();

private:
    DataChangeBus();
};
