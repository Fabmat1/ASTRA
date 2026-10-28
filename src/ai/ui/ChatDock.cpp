#include "ai/ui/ChatDock.h"

#include "ai/AiHost.h"
#include "ai/AiRuntime.h"
#include "ai/AiSettings.h"
#include "ai/ChatSession.h"
#include "ai/PermissionGate.h"
#include "ai/ToolRegistry.h"
#include "ai/ui/AiController.h"
#include "ai/ui/GuiAiHost.h"
#include "ai/ui/UiWindows.h"
#include "app/ui/MainWindow.h"
#include "app/ui/ProjectView.h"
#include "app/ui/UiIcons.h"
#include "app/ui/panels/PanelUtils.h"
#include "core/Project.h"
#include "core/Star.h"

#include <QApplication>
#include <QBuffer>
#include <QDesktopServices>
#include <QFileInfo>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QPushButton>
#include <QScrollBar>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <cmath>

namespace astra::ai::ui {

namespace {
QString esc(const QString& s) { return s.toHtmlEscaped(); }

/// Serves the images agents produced from the dock's own store, so they
/// survive the document being rebuilt on every render.
class ChatView : public QTextBrowser {
public:
    explicit ChatView(const QHash<QString, QImage>* images) : _images(images) {}
    QVariant loadResource(int type, const QUrl& name) override
    {
        if (type == QTextDocument::ImageResource && name.scheme() == QLatin1String("astra"))
            return _images->value(name.path());
        return QTextBrowser::loadResource(type, name);
    }
private:
    const QHash<QString, QImage>* _images;
};

QString markdownToHtml(const QString& md)
{
    QTextDocument d;
    d.setMarkdown(md);
    QString html = d.toHtml();
    // Keep only the body: the fragments are concatenated into one document.
    const int b = html.indexOf(QLatin1String("<body"));
    const int bs = html.indexOf(QLatin1Char('>'), b) + 1;
    const int be = html.lastIndexOf(QLatin1String("</body>"));
    return (b >= 0 && be > bs) ? html.mid(bs, be - bs) : html;
}

bool isImageFile(const QString& path)
{
    return !QImageReader::imageFormat(path).isEmpty();
}

QByteArray toPng(const QImage& img)
{
    // Models read images up to about 1.5 megapixels; larger ones only cost.
    QImage scaled = img;
    const double mp = double(img.width()) * img.height() / 1.0e6;
    if (mp > 1.5)
        scaled = img.scaled(img.size() * std::sqrt(1.5 / mp), Qt::KeepAspectRatio, Qt::SmoothTransformation);
    QByteArray bytes;
    QBuffer buf(&bytes);
    buf.open(QIODevice::WriteOnly);
    scaled.save(&buf, "PNG");
    return bytes;
}

/// A themed glyph in one colour, for icons sitting on the accent fill.
QIcon tinted(UiIcons::Role role, int px, const QColor& color)
{
    QPixmap src = UiIcons::icon(role, px).pixmap(px, px);
    QPixmap out(src.size());
    out.setDevicePixelRatio(src.devicePixelRatio());
    out.fill(Qt::transparent);
    QPainter p(&out);
    p.drawPixmap(0, 0, src);
    p.setCompositionMode(QPainter::CompositionMode_SourceIn);
    p.fillRect(out.rect(), color);
    p.end();
    return QIcon(out);
}

struct ModeLook {
    QString label, tip;
    QColor  color;
    bool    bold = false, italic = false;
};

ModeLook modeLook(const QString& mode)
{
    if (mode == QLatin1String("auto"))
        return {QObject::tr("Auto"),
                QObject::tr("Auto: everything runs without asking (a reviewer model can check risky actions)"),
                QColor(QStringLiteral("#d35400")), true, false};
    if (mode == QLatin1String("readonly"))
        return {QObject::tr("Read-only"),
                QObject::tr("Read-only: the assistant can look at your data and windows but change nothing"),
                PanelUtils::towardBg(PanelUtils::themeFg(), 0.45), false, true};
    return {QObject::tr("Ask first"),
            QObject::tr("Ask first: deleting data, running code and remote jobs wait for your OK"),
            PanelUtils::themeAccent(), false, false};
}
} // namespace

// ── ChatInput ────────────────────────────────────────────────────────────────

ChatInput::ChatInput(QWidget* parent) : QPlainTextEdit(parent)
{
    setTabChangesFocus(true);
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    connect(this, &QPlainTextEdit::textChanged, this, &ChatInput::fitHeight);
    fitHeight();
}

QSize ChatInput::sizeHint() const
{
    // QPlainTextEdit's document height is in lines. Around the text sit the
    // document margin and whatever frame and padding the theme gives the
    // widget, which is what the viewport is short of the whole widget.
    const int lines = std::clamp(int(document()->size().height()), 1, 8);
    const int chrome = std::max(4, height() - viewport()->height());
    return {200, lines * fontMetrics().lineSpacing() + int(std::ceil(document()->documentMargin() * 2)) + chrome};
}

void ChatInput::fitHeight()
{
    // Grow with the text; scroll only once it is longer than the box gets.
    const bool full = document()->size().height() > 8;
    setVerticalScrollBarPolicy(full ? Qt::ScrollBarAsNeeded : Qt::ScrollBarAlwaysOff);
    setFixedHeight(sizeHint().height());
}

void ChatInput::showEvent(QShowEvent* e)
{
    QPlainTextEdit::showEvent(e);
    fitHeight();
}

void ChatInput::changeEvent(QEvent* e)
{
    QPlainTextEdit::changeEvent(e);
    // A theme or font change alters the padding and the line height.
    if (e->type() == QEvent::StyleChange || e->type() == QEvent::FontChange) fitHeight();
}

void ChatInput::keyPressEvent(QKeyEvent* e)
{
    if ((e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) &&
        !(e->modifiers() & (Qt::ShiftModifier | Qt::AltModifier))) {
        emit sendRequested();
        return;
    }
    QPlainTextEdit::keyPressEvent(e);
}

bool ChatInput::canInsertFromMimeData(const QMimeData* source) const
{
    return source->hasImage() || source->hasUrls() || QPlainTextEdit::canInsertFromMimeData(source);
}

void ChatInput::insertFromMimeData(const QMimeData* source)
{
    // A screenshot or a copied image: attach it.
    if (source->hasImage()) {
        const QImage img = qvariant_cast<QImage>(source->imageData());
        if (!img.isNull()) { emit imageAdded(img); return; }
    }
    // Files: images are attached, anything else goes in as its path.
    if (source->hasUrls()) {
        QStringList paths;
        for (const QUrl& u : source->urls()) {
            if (!u.isLocalFile()) { paths << u.toString(); continue; }
            const QString p = u.toLocalFile();
            if (isImageFile(p)) {
                const QImage img(p);
                if (!img.isNull()) { emit imageAdded(img); continue; }
            }
            paths << p;
        }
        if (!paths.isEmpty()) insertPlainText(paths.join(QLatin1Char('\n')));
        return;
    }
    // Text, as plain text: no formatting rides along from web pages or PDFs.
    if (source->hasText()) {
        insertPlainText(source->text());
        return;
    }
    QPlainTextEdit::insertFromMimeData(source);
}

// ── ChatDock ─────────────────────────────────────────────────────────────────

ChatDock::ChatDock(AiController* controller, QWidget* parent)
    : QDockWidget(tr("AI Assistant"), parent), _controller(controller)
{
    setObjectName(QStringLiteral("aiChatDock"));
    setProperty("astra.internal", true);   // agents read the chat through their own context
    setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    _session = new ChatSession(controller->runtime(), this);

    auto* root = new QWidget;
    auto* lay = new QVBoxLayout(root);
    lay->setContentsMargins(8, 4, 8, 8);
    lay->setSpacing(6);

    // Header: the model in use, history and a new chat.
    auto* header = new QHBoxLayout;
    header->setSpacing(2);
    _modelLabel = new QLabel;
    _modelLabel->setObjectName(QStringLiteral("aiChatModel"));
    _modelLabel->setTextFormat(Qt::PlainText);
    header->addWidget(_modelLabel, 1);
    _historyBtn = new QToolButton;
    _historyBtn->setAutoRaise(true);
    _historyBtn->setToolTip(tr("Earlier conversations"));
    UiIcons::apply(_historyBtn, UiIcons::Role::History, 18);
    header->addWidget(_historyBtn);
    _newBtn = new QToolButton;
    _newBtn->setAutoRaise(true);
    _newBtn->setToolTip(tr("New conversation"));
    UiIcons::apply(_newBtn, UiIcons::Role::NewChat, 18);
    header->addWidget(_newBtn);
    lay->addLayout(header);

    // Body: the conversation and the composer; the setup card floats on top.
    _body = new QWidget;
    auto* bodyLay = new QVBoxLayout(_body);
    bodyLay->setContentsMargins(0, 0, 0, 0);
    _chatPage = new QWidget;
    auto* chatLay = new QVBoxLayout(_chatPage);
    chatLay->setContentsMargins(0, 0, 0, 0);
    chatLay->setSpacing(6);
    _view = new ChatView(&_images);
    _view->setOpenLinks(false);
    _view->setFrameShape(QFrame::NoFrame);
    chatLay->addWidget(_view, 1);

    auto* composer = new QFrame;
    composer->setObjectName(QStringLiteral("aiComposer"));
    auto* compLay = new QVBoxLayout(composer);
    compLay->setContentsMargins(8, 6, 6, 6);
    compLay->setSpacing(4);
    _attachBar = new QWidget;
    _attachLayout = new QHBoxLayout(_attachBar);
    _attachLayout->setContentsMargins(0, 0, 0, 0);
    _attachLayout->setSpacing(4);
    _attachLayout->addStretch();
    _attachBar->setVisible(false);
    compLay->addWidget(_attachBar);
    _input = new ChatInput;
    _input->setObjectName(QStringLiteral("aiInput"));
    _input->setPlaceholderText(tr("Ask about your stars, or tell ASTRA what to do"));
    compLay->addWidget(_input);
    auto* controls = new QHBoxLayout;
    controls->setSpacing(6);
    _modePill = new QToolButton;
    _modePill->setObjectName(QStringLiteral("aiModePill"));
    _modePill->setCursor(Qt::PointingHandCursor);
    controls->addWidget(_modePill);
    _usageLabel = new QLabel;
    _usageLabel->setObjectName(QStringLiteral("aiChatUsage"));
    controls->addWidget(_usageLabel, 1);
    _sendBtn = new QToolButton;
    _sendBtn->setObjectName(QStringLiteral("aiSend"));
    _sendBtn->setFixedSize(30, 30);
    controls->addWidget(_sendBtn);
    compLay->addLayout(controls);
    chatLay->addWidget(composer);
    bodyLay->addWidget(_chatPage);

    _setupCard = new QFrame(_body);
    _setupCard->setObjectName(QStringLiteral("aiSetupCard"));
    auto* cardLay = new QVBoxLayout(_setupCard);
    cardLay->setContentsMargins(18, 16, 18, 16);
    cardLay->setSpacing(10);
    auto* cardTitle = new QLabel(tr("Set up the AI assistant"));
    QFont tf = cardTitle->font();
    tf.setBold(true);
    tf.setPointSizeF(tf.pointSizeF() * 1.15);
    cardTitle->setFont(tf);
    cardLay->addWidget(cardTitle);
    auto* cardText = new QLabel(
        tr("Choose a provider (Anthropic, OpenAI, Google, or your own endpoint such as a university "
           "proxy or a local model), enter its API key and pick a model. The assistant can then read "
           "your ASTRA data and work in ASTRA's windows with you; risky actions ask you first."));
    cardText->setWordWrap(true);
    cardLay->addWidget(cardText);
    auto* openSettings = new QPushButton(tr("Open AI settings"));
    openSettings->setDefault(true);
    cardLay->addWidget(openSettings, 0, Qt::AlignLeft);
    auto* shadow = new QGraphicsDropShadowEffect(_setupCard);
    shadow->setBlurRadius(24);
    shadow->setOffset(0, 4);
    shadow->setColor(QColor(0, 0, 0, 70));
    _setupCard->setGraphicsEffect(shadow);
    _body->installEventFilter(this);

    lay->addWidget(_body, 1);
    setWidget(root);

    _renderTimer = new QTimer(this);
    _renderTimer->setSingleShot(true);
    _renderTimer->setInterval(60);
    connect(_renderTimer, &QTimer::timeout, this, &ChatDock::render);

    AiSettings* s = controller->settings();
    connect(s, &AiSettings::changed, this, &ChatDock::updateState);
    connect(openSettings, &QPushButton::clicked, this, [this] {
        if (auto* w = _controller->host()->window())
            QMetaObject::invokeMethod(w, "showSettings", Q_ARG(QString, QStringLiteral("AI")));
    });
    connect(_sendBtn, &QToolButton::clicked, this, [this] {
        if (_session->busy()) stop(); else send();
    });
    connect(_input, &ChatInput::sendRequested, this, &ChatDock::send);
    connect(_input, &ChatInput::imageAdded, this, &ChatDock::addAttachment);
    connect(_modePill, &QToolButton::clicked, this, &ChatDock::showModeMenu);
    connect(_newBtn, &QToolButton::clicked, this, &ChatDock::newChat);
    connect(_historyBtn, &QToolButton::clicked, this, &ChatDock::showHistoryMenu);
    connect(_view, &QTextBrowser::anchorClicked, this, [this](const QUrl& url) {
        if (url.scheme() == QLatin1String("tool")) {
            const int i = _toolEntry.value(url.path(), -1);
            if (i >= 0) { _entries[size_t(i)].expanded = !_entries[size_t(i)].expanded; render(); }
        } else if (url.scheme() == QLatin1String("http") || url.scheme() == QLatin1String("https")) {
            QDesktopServices::openUrl(url);
        }
    });

    connect(_session, &ChatSession::busyChanged, this, &ChatDock::updateState);
    connect(_session, &ChatSession::assistantStarted, this, [this] {
        _entries.push_back({Entry::Kind::Assistant});
        scheduleRender();
    });
    connect(_session, &ChatSession::assistantText, this, [this](const QString& d) {
        if (_entries.empty() || _entries.back().kind != Entry::Kind::Assistant)
            _entries.push_back({Entry::Kind::Assistant});
        _entries.back().text += d;
        scheduleRender();
    });
    connect(_session, &ChatSession::assistantFinished, this, [this](const QString& full) {
        if (!_entries.empty() && _entries.back().kind == Entry::Kind::Assistant) {
            _entries.back().text = full;
            if (full.isEmpty()) _entries.pop_back();
        }
        scheduleRender();
    });
    connect(_session, &ChatSession::toolStarted, this,
            [this](const QString& id, const QString& tool, const QJsonObject& args) {
        Entry e{Entry::Kind::Tool};
        e.toolId = id;
        e.tool = tool;
        e.args = QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Indented));
        if (const ToolDef* def = _controller->runtime()->tools()->find(tool))
            e.text = PermissionGate::summarize(*def, args);
        _toolEntry.insert(id, int(_entries.size()));
        _entries.push_back(e);
        scheduleRender();
    });
    connect(_session, &ChatSession::toolFinished, this,
            [this](const QString& id, const QString&, const ToolResult& r) {
        const int i = _toolEntry.value(id, -1);
        if (i < 0) return;
        Entry& e = _entries[size_t(i)];
        e.toolDone = true;
        e.toolOk = !r.isError;
        e.result = r.plainText().left(4000);
        for (const auto& v : r.content) {
            const QJsonObject b = v.toObject();
            if (b.value(QStringLiteral("type")).toString() != QLatin1String("image")) continue;
            const QImage img = QImage::fromData(QByteArray::fromBase64(b.value(QStringLiteral("data")).toString().toLatin1()));
            if (!img.isNull()) e.images << addImage(img);
        }
        scheduleRender();
    });
    connect(_session, &ChatSession::errorOccurred, this, [this](const QString& m) {
        _entries.push_back({Entry::Kind::Error, m});
        scheduleRender();
    });
    connect(_session, &ChatSession::usageChanged, this, [this](const LlmUsage& u) {
        QString t = tr("%1k in, %2k out").arg(u.inputTokens / 1000.0, 0, 'f', 1).arg(u.outputTokens / 1000.0, 0, 'f', 1);
        if (u.costUsd >= 0) t += tr(", $%1").arg(u.costUsd, 0, 'f', 3);
        _usageLabel->setText(t);
    });

    updateState();
}

bool ChatDock::eventFilter(QObject* o, QEvent* e)
{
    if (o == _body && (e->type() == QEvent::Resize || e->type() == QEvent::Show)) placeSetupCard();
    return QDockWidget::eventFilter(o, e);
}

void ChatDock::changeEvent(QEvent* e)
{
    QDockWidget::changeEvent(e);
    // A new theme: colours are derived from it, so restyle (once; restyling
    // itself changes the palette).
    if (e->type() == QEvent::PaletteChange && !_restyling) {
        _restyling = true;
        updateState();
        _restyling = false;
    }
}

void ChatDock::placeSetupCard()
{
    const int w = std::max(220, std::min(_body->width() - 32, 380));
    _setupCard->setFixedWidth(w);
    // Wrapped text needs its height for this width, not the unwrapped hint.
    QLayout* l = _setupCard->layout();
    _setupCard->setFixedHeight(l->hasHeightForWidth() ? l->totalHeightForWidth(w) : l->totalSizeHint().height());
    _setupCard->move((_body->width() - _setupCard->width()) / 2,
                     std::max(16, (_body->height() - _setupCard->height()) / 3));
    _setupCard->raise();
}

void ChatDock::focusInput()
{
    if (_ready) _input->setFocus();
}

void ChatDock::stop()
{
    _session->stop();
}

void ChatDock::updateModePill()
{
    const ModeLook look = modeLook(_controller->settings()->permissionMode());
    _modePill->setText(look.label);
    _modePill->setToolTip(look.tip + tr(". Click to change."));
    QColor bg = look.color;
    bg.setAlphaF(0.12);
    QColor edge = look.color;
    edge.setAlphaF(0.55);
    _modePill->setStyleSheet(QStringLiteral(
        "QToolButton#aiModePill { color: %1; border: 1px solid %2; border-radius: 9px; padding: 1px 9px;"
        " background: %3; font-weight: %4; font-style: %5; }")
        .arg(look.color.name(), edge.name(QColor::HexArgb), bg.name(QColor::HexArgb),
             look.bold ? QStringLiteral("bold") : QStringLiteral("normal"),
             look.italic ? QStringLiteral("italic") : QStringLiteral("normal")));
}

void ChatDock::showModeMenu()
{
    QMenu menu(this);
    AiSettings* s = _controller->settings();
    for (const QString& mode : {QStringLiteral("ask"), QStringLiteral("auto"), QStringLiteral("readonly")}) {
        const ModeLook look = modeLook(mode);
        QAction* a = menu.addAction(look.tip);
        a->setCheckable(true);
        a->setChecked(s->permissionMode() == mode);
        QFont f = a->font();
        f.setBold(look.bold);
        f.setItalic(look.italic);
        a->setFont(f);
        connect(a, &QAction::triggered, this, [s, mode] { s->setPermissionMode(mode); });
    }
    menu.exec(_modePill->mapToGlobal(QPoint(0, -menu.sizeHint().height() - 4)));
}

void ChatDock::updateState()
{
    AiSettings* s = _controller->settings();
    const ProviderConfig p = s->chatProvider();
    _ready = p.isConfigured() && (!s->apiKey(p.keyRef).isEmpty() || p.preset == QLatin1String("custom"));

    // Not set up: the conversation area stays in view, faded behind the card.
    _setupCard->setVisible(!_ready);
    _chatPage->setEnabled(_ready);
    if (!_ready) {
        auto* fade = new QGraphicsOpacityEffect(_chatPage);
        fade->setOpacity(0.35);
        _chatPage->setGraphicsEffect(fade);
        placeSetupCard();
    } else {
        _chatPage->setGraphicsEffect(nullptr);
    }
    _historyBtn->setEnabled(_ready);
    _newBtn->setEnabled(_ready);
    _modelLabel->setText(_ready ? QStringLiteral("%1 · %2").arg(ProviderConfig::presetLabel(p.preset), p.model)
                                : tr("Not set up"));

    const bool busy = _session->busy();
    _sendBtn->setIcon(tinted(busy ? UiIcons::Role::Stop : UiIcons::Role::Send, 16, PanelUtils::themeAccentText()));
    _sendBtn->setIconSize(QSize(16, 16));
    _sendBtn->setToolTip(busy ? tr("Stop") : tr("Send (Enter)"));

    // Theme-derived styling, refreshed here so a theme change follows.
    const QColor accent = PanelUtils::themeAccent();
    const QColor surface = PanelUtils::themeSurface();
    const QColor border = PanelUtils::towardBg(PanelUtils::themeFg(), 0.75);
    const QColor muted = PanelUtils::towardBg(PanelUtils::themeFg(), 0.4);
    setStyleSheet(QStringLiteral(
        "QFrame#aiComposer { border: 1px solid %1; border-radius: 10px; background: %2; }"
        "QPlainTextEdit#aiInput { border: none; background: transparent; padding: 0px; }"
        "QFrame#aiSetupCard { border: 1px solid %1; border-radius: 12px; background: %2; }"
        "QToolButton#aiSend { border-radius: 15px; background: %3; border: none; }"
        "QToolButton#aiSend:disabled { background: %1; }"
        "QLabel#aiChatModel, QLabel#aiChatUsage { color: %4; }")
        .arg(border.name(), surface.name(), accent.name(), muted.name()));
    updateModePill();
    render();
}

QString ChatDock::currentContext() const
{
    QStringList lines;
    GuiAiHost* host = _controller->host();
    if (auto p = host->currentProject()) {
        lines << QStringLiteral("Open project: %1 (id %2, %3 stars)").arg(p->getName(), p->getId()).arg(p->getStarCount());
        if (auto* pv = host->window()->projectView()) {
            const auto sel = pv->getSelectedStars();
            if (!sel.empty()) {
                QStringList names;
                for (size_t i = 0; i < sel.size() && i < 10; ++i)
                    names << QStringLiteral("%1 (%2)").arg(sel[i]->getAlias().isEmpty() ? sel[i]->getSourceId()
                                                                                        : sel[i]->getAlias(),
                                                           sel[i]->getId());
                lines << QStringLiteral("Selected stars (%1): %2%3").arg(sel.size()).arg(names.join(QStringLiteral(", ")),
                                                                        sel.size() > 10 ? QStringLiteral(", ...") : QString());
            }
        }
    } else {
        lines << QStringLiteral("No project is open (project overview).");
    }
    QStringList wins;
    for (QWidget* w : visibleWindows())
        if (windowId(w) != QLatin1String("main")) wins << QStringLiteral("%1 [%2]").arg(w->windowTitle(), windowId(w));
    if (!wins.isEmpty()) lines << QStringLiteral("Other open windows: %1").arg(wins.join(QStringLiteral("; ")));
    return lines.join(QLatin1Char('\n'));
}

QString ChatDock::addImage(const QImage& image)
{
    QImage img = image;
    if (img.width() > 520) img = img.scaledToWidth(520, Qt::SmoothTransformation);
    const QString name = QStringLiteral("img%1").arg(++_imageCounter);
    _images.insert(name, img);
    return name;
}

void ChatDock::addAttachment(const QImage& image)
{
    if (image.isNull()) return;
    _attachments << image;
    auto* chip = new QToolButton;
    chip->setAutoRaise(true);
    chip->setIcon(QPixmap::fromImage(image.scaled(48, 48, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
    chip->setIconSize(QSize(40, 40));
    chip->setToolTip(tr("%1 x %2 px image; click to remove").arg(image.width()).arg(image.height()));
    chip->setProperty("attachIndex", int(_attachments.size()) - 1);
    connect(chip, &QToolButton::clicked, this, [this, chip] {
        const int i = chip->property("attachIndex").toInt();
        if (i >= 0 && i < _attachments.size()) _attachments[i] = QImage();
        chip->deleteLater();
        bool any = false;
        for (const QImage& a : std::as_const(_attachments)) any |= !a.isNull();
        if (!any) clearAttachments();
    });
    _attachLayout->insertWidget(_attachLayout->count() - 1, chip);
    _attachBar->setVisible(true);
    _input->setFocus();
}

void ChatDock::clearAttachments()
{
    _attachments.clear();
    while (_attachLayout->count() > 1) {
        QLayoutItem* it = _attachLayout->takeAt(0);
        if (it->widget()) it->widget()->deleteLater();
        delete it;
    }
    _attachBar->setVisible(false);
}

void ChatDock::send()
{
    if (!_ready || _session->busy()) return;
    const QString text = _input->toPlainText().trimmed();
    QList<QByteArray> images;
    Entry user{Entry::Kind::User, text};
    for (const QImage& a : std::as_const(_attachments)) {
        if (a.isNull()) continue;
        images << toPng(a);
        user.images << addImage(a);
    }
    if (text.isEmpty() && images.isEmpty()) return;
    _input->clear();
    clearAttachments();
    _entries.push_back(user);
    render();
    _session->send(text, currentContext(), images);
}

void ChatDock::newChat()
{
    _session->clear();
    _entries.clear();
    _toolEntry.clear();
    _usageLabel->clear();
    clearAttachments();
    render();
    focusInput();
}

void ChatDock::showHistoryMenu()
{
    QMenu menu(this);
    const QJsonArray convs = ChatSession::savedConversations();
    if (convs.isEmpty()) menu.addAction(tr("No earlier conversations"))->setEnabled(false);
    for (const auto& v : convs) {
        const QJsonObject o = v.toObject();
        const QString title = o.value(QStringLiteral("title")).toString();
        const QString when = o.value(QStringLiteral("updated")).toString().left(16).replace(QLatin1Char('T'), QLatin1Char(' '));
        const QString id = o.value(QStringLiteral("id")).toString();
        menu.addAction(QStringLiteral("%1   %2").arg(when, title.isEmpty() ? tr("(untitled)") : title.left(50)),
                       this, [this, id] { loadConversation(id); });
    }
    menu.exec(_historyBtn->mapToGlobal(QPoint(0, _historyBtn->height())));
}

void ChatDock::loadConversation(const QString& id)
{
    if (!_session->load(id)) return;
    _entries.clear();
    _toolEntry.clear();
    for (const auto& m : _session->messages()) {
        for (const auto& v : m.content) {
            const QJsonObject b = v.toObject();
            const QString type = b.value(QStringLiteral("type")).toString();
            if (type == QLatin1String("text")) {
                const QString t = b.value(QStringLiteral("text")).toString();
                if (t.startsWith(QLatin1String("<astra_context>"))) continue;
                _entries.push_back({m.role == QLatin1String("user") ? Entry::Kind::User : Entry::Kind::Assistant, t});
            } else if (type == QLatin1String("tool_use")) {
                Entry e{Entry::Kind::Tool};
                e.toolId = b.value(QStringLiteral("id")).toString();
                e.tool = b.value(QStringLiteral("name")).toString();
                e.args = QString::fromUtf8(QJsonDocument(b.value(QStringLiteral("input")).toObject()).toJson(QJsonDocument::Indented));
                e.toolDone = true;
                _toolEntry.insert(e.toolId, int(_entries.size()));
                _entries.push_back(e);
            } else if (type == QLatin1String("tool_result")) {
                const int i = _toolEntry.value(b.value(QStringLiteral("tool_use_id")).toString(), -1);
                if (i < 0) continue;
                QString text;
                for (const auto& iv : b.value(QStringLiteral("content")).toArray())
                    text += iv.toObject().value(QStringLiteral("text")).toString();
                _entries[size_t(i)].result = text.left(4000);
                _entries[size_t(i)].toolOk = !b.value(QStringLiteral("is_error")).toBool();
            }
        }
    }
    _entries.push_back({Entry::Kind::Note, tr("Conversation restored. Images are not kept in saved chats.")});
    render();
}

void ChatDock::scheduleRender()
{
    if (!_renderTimer->isActive()) _renderTimer->start();
}

QString ChatDock::entryHtml(const Entry& e) const
{
    const QColor muted = PanelUtils::towardBg(PanelUtils::themeFg(), 0.4);
    switch (e.kind) {
    case Entry::Kind::User: {
        QString imgs;
        for (const QString& img : e.images) imgs += QStringLiteral("<img src='astra:%1' width='160'> ").arg(img);
        return QStringLiteral("<table width='100%' cellspacing='0' cellpadding='8' style='margin-top:8px'>"
                              "<tr><td bgcolor='%1'>%2%3</td></tr></table>")
            .arg(PanelUtils::towardBg(PanelUtils::themeAccent(), 0.85).name(),
                 imgs.isEmpty() ? QString() : imgs + QStringLiteral("<br>"),
                 esc(e.text).replace(QLatin1Char('\n'), QLatin1String("<br>")));
    }
    case Entry::Kind::Assistant:
        return QStringLiteral("<div style='margin-top:6px'>%1</div>")
            .arg(markdownToHtml(e.text.isEmpty() ? QStringLiteral("...") : e.text));
    case Entry::Kind::Error:
        return QStringLiteral("<p style='color:#c0392b'>%1</p>").arg(esc(e.text));
    case Entry::Kind::Note:
        return QStringLiteral("<p style='color:%1'><i>%2</i></p>").arg(muted.name(), esc(e.text));
    case Entry::Kind::Tool: {
        const QString state = !e.toolDone ? tr("running") : e.toolOk ? QStringLiteral("✓") : QStringLiteral("✗");
        QString line = QStringLiteral("<p style='color:%1; margin:2px 0'><a href='tool:%2' style='color:%1'>%3</a> %4 %5</p>")
                           .arg(muted.name(), e.toolId, esc(e.tool), esc(e.text.left(160)), state);
        if (e.expanded)
            line += QStringLiteral("<pre style='font-size:small'>%1</pre><pre style='font-size:small'>%2</pre>")
                        .arg(esc(e.args), esc(e.result));
        for (const QString& img : e.images)
            line += QStringLiteral("<p><img src='astra:%1'></p>").arg(img);
        return line;
    }
    }
    return {};
}

QString ChatDock::exampleHtml() const
{
    // What a conversation looks like, shown faded behind the setup card.
    std::vector<Entry> demo;
    demo.push_back({Entry::Kind::User, tr("Which stars in this project have an RV semi-amplitude above 100 km/s?")});
    Entry tool{Entry::Kind::Tool};
    tool.tool = QStringLiteral("query_stars");
    tool.text = tr("filter rv_k > 100");
    tool.toolDone = true;
    demo.push_back(tool);
    demo.push_back({Entry::Kind::Assistant,
                    tr("Three stars qualify:\n\n| Star | K [km/s] | P [d] |\n|---|---|---|\n"
                       "| PG 1232-136 | 129.6 | 0.363 |\n| GALEX J0812+3511 | 112.4 | 0.191 |\n"
                       "| FBS 2253+335 | 104.0 | 0.508 |\n\nShall I open their detail windows?")});
    demo.push_back({Entry::Kind::User, tr("Yes, and zoom the light curve to the eclipse.")});
    QString html;
    for (const Entry& e : demo) html += entryHtml(e);
    return html;
}

void ChatDock::render()
{
    QScrollBar* bar = _view->verticalScrollBar();
    const bool atBottom = bar->value() >= bar->maximum() - 40;
    QString html;
    if (!_ready) {
        html = exampleHtml();
    } else if (_entries.empty()) {
        html = QStringLiteral("<p style='color:%1'>%2</p>").arg(
            PanelUtils::towardBg(PanelUtils::themeFg(), 0.4).name(),
            tr("Ask about your data (\"which stars here have RV amplitudes above 100 km/s?\"), or ask ASTRA to "
               "do something (\"open the selected star and zoom the light curve to the eclipse\"). The assistant "
               "sees which project is open and which stars are selected. You can paste or drop images in."));
    }
    for (const Entry& e : _entries) html += entryHtml(e);
    _view->setHtml(html);
    if (atBottom) bar->setValue(bar->maximum());
}

} // namespace astra::ai::ui
