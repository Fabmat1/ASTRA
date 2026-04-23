#pragma once

#include <QDialog>
#include <QProcess>
#include <QTimer>
#include "fitting/FitTypes.h"
#include "views/widgets/IsisMacroPanel.h"

class QCheckBox;
class QLabel;
class QPushButton;
class TerminalView;

class InteractiveIsisDialog : public QDialog
{
    Q_OBJECT
public:
    explicit InteractiveIsisDialog(const astra::fitting::SpectralFitJob& job,
                                    QWidget* parent = nullptr);
    ~InteractiveIsisDialog() override;

    static QString generateScript(const astra::fitting::SpectralFitJob& job,
                                   const QString& workDir);

protected:
    void closeEvent(QCloseEvent* e) override;

signals:
    void fitExtracted(const astra::fitting::SpectralFitResult& result,
                       const astra::fitting::SpectralFitJob&   job);

private slots:
    void onStart();
    void onStop();
    void onReadyRead();
    void onProcessFinished(int exitCode, QProcess::ExitStatus status);
    void onLineEntered(const QString& line);
    void onShowScript();
    void onExtractFit();
    void checkExtractAvailable();


private:
    static QString resolveBinary();
    static QString generateHeader(const astra::fitting::SpectralFitJob& job,
                                   const QString& workDir);
    void appendStatus(const QString& line);
    void sendCommands(const QStringList& commands);

    astra::fitting::SpectralFitJob _job;
    QString   _workDir;
    QString   _scriptPath;
    QProcess* _proc     = nullptr;
    IsisMacroPanel* _macros       = nullptr;
    QCheckBox*      _hotkeyToggle = nullptr;

    TerminalView* _term     = nullptr;
    QLabel*       _status   = nullptr;
    QPushButton*  _startBtn = nullptr;
    QPushButton*  _stopBtn  = nullptr;
    QPushButton*  _showBtn  = nullptr;
    QPushButton*  _closeBtn = nullptr;
    QPushButton* _extractBtn  = nullptr;
    QTimer*      _extractPoll = nullptr;
};