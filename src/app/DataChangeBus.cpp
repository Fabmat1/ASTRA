#include "app/DataChangeBus.h"

#include <QCoreApplication>
#include <QMetaObject>
#include <QThread>

DataChangeBus& DataChangeBus::instance()
{
    static DataChangeBus* bus = new DataChangeBus;
    return *bus;
}

DataChangeBus::DataChangeBus()
{
    if (auto* app = QCoreApplication::instance()) moveToThread(app->thread());
}

namespace {
template <typename Fn>
void onMainThread(DataChangeBus* bus, Fn fn)
{
    if (QThread::currentThread() == bus->thread()) fn();
    else QMetaObject::invokeMethod(bus, std::move(fn), Qt::QueuedConnection);
}
} // namespace

void DataChangeBus::postStarChanged(const QString& projectId, const QString& starId,
                                    const QStringList& fields)
{
    onMainThread(this, [=, this] { emit starChanged(projectId, starId, fields); });
}

void DataChangeBus::postStarsAdded(const QString& projectId, const QStringList& starIds)
{
    onMainThread(this, [=, this] { emit starsAdded(projectId, starIds); });
}

void DataChangeBus::postStarsRemoved(const QString& projectId, const QStringList& starIds)
{
    onMainThread(this, [=, this] { emit starsRemoved(projectId, starIds); });
}

void DataChangeBus::postStarDataChanged(const QString& projectId, const QString& starId,
                                        const QString& kind)
{
    onMainThread(this, [=, this] { emit starDataChanged(projectId, starId, kind); });
}

void DataChangeBus::postProjectsChanged()
{
    onMainThread(this, [this] { emit projectsChanged(); });
}
