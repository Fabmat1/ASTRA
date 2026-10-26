#include "sed/SedGridBroker.h"

#include <specfit/GridDataProvider.hpp>

#include <QFutureWatcher>
#include <QtConcurrent>

#include <exception>
#include <string>
#include <vector>

namespace {

const QByteArray kPrefix = QByteArrayLiteral("@@SEDFIT_GRID\t");

struct Answer {
    QByteArray line;   // reply for sedfit, without the newline
    QString    note;   // shown in the fit log, may be empty
    int        served = 0;
};

Answer answerExists(const std::string& path)
{
    auto provider = specfit::grid_data_provider();
    const bool ok = provider && provider->handles(path) &&
                    provider->exists(path);
    return {ok ? QByteArrayLiteral("1") : QByteArrayLiteral("0"), {}};
}

Answer answerGet(const std::vector<std::string>& paths)
{
    auto provider = specfit::grid_data_provider();
    if (!provider)
        return {QByteArrayLiteral("ERR\tno grid streaming provider installed"),
                {}};
    for (const auto& p : paths)
        if (!provider->handles(p))
            return {"ERR\tnot a streamed grid path: " + QByteArray::fromStdString(p),
                    {}};
    try {
        provider->prefetch(paths);
        QByteArray line = QByteArrayLiteral("OK");
        for (const auto& p : paths)
            line += '\t' + QByteArray::fromStdString(provider->localize(p));
        return {line, {}, int(paths.size())};
    } catch (const std::exception& e) {
        QByteArray msg(e.what());
        msg.replace('\t', ' ').replace('\n', ' ');
        return {"ERR\t" + msg, QString::fromUtf8(msg)};
    }
}

} // namespace

SedGridBroker::SedGridBroker(QProcess* proc, QObject* parent)
    : QObject(parent)
    , _proc(proc)
{
    connect(proc, &QProcess::readyReadStandardOutput,
            this, &SedGridBroker::onReadyRead);
}

void SedGridBroker::enableIn(QProcessEnvironment& env)
{
    env.insert(QStringLiteral("SEDFIT_GRID_BROKER"), QStringLiteral("stdio"));
}

void SedGridBroker::flush()
{
    if (_proc) _buf += _proc->readAllStandardOutput();
    if (!_buf.isEmpty()) emit output(QString::fromLocal8Bit(_buf));
    _buf.clear();
    if (_served > 0)
        emit output(QStringLiteral("Streamed grid files used: %1")
                        .arg(_served));
    _served = 0;
}

void SedGridBroker::onReadyRead()
{
    if (!_proc) return;
    _buf += _proc->readAllStandardOutput();

    QByteArray text;
    int nl;
    while ((nl = _buf.indexOf('\n')) >= 0) {
        QByteArray line = _buf.left(nl);
        _buf.remove(0, nl + 1);
        if (line.startsWith(kPrefix)) {
            if (!text.isEmpty()) emit output(QString::fromLocal8Bit(text));
            text.clear();
            handleRequest(line.mid(kPrefix.size()));
        } else {
            if (!text.isEmpty()) text += '\n';
            text += line;
        }
    }
    if (!text.isEmpty()) emit output(QString::fromLocal8Bit(text));
}

void SedGridBroker::handleRequest(const QByteArray& line)
{
    const QList<QByteArray> f = line.split('\t');
    const QByteArray verb = f.value(0);
    std::vector<std::string> paths;
    for (int i = 1; i < f.size(); ++i) paths.push_back(f[i].toStdString());

    if ((verb != "EXISTS" && verb != "GET") || paths.empty()) {
        reply(QByteArrayLiteral("ERR\tmalformed request"));
        return;
    }

    /*  The provider blocks on the network; sedfit waits for the reply
     *  anyway, so the GUI thread only must not.                            */
    auto* watcher = new QFutureWatcher<Answer>(this);
    connect(watcher, &QFutureWatcher<Answer>::finished, this, [this, watcher] {
        const Answer a = watcher->result();
        watcher->deleteLater();
        if (!a.note.isEmpty()) emit output(a.note);
        _served += a.served;
        reply(a.line);
    });
    const bool exists = verb == "EXISTS";
    watcher->setFuture(QtConcurrent::run([exists, paths] {
        return exists ? answerExists(paths.front()) : answerGet(paths);
    }));
}

void SedGridBroker::reply(const QByteArray& line)
{
    if (!_proc || _proc->state() != QProcess::Running) return;
    _proc->write(line + '\n');
}
