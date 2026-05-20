#include "ProjectSelectionView.h"
#include "controllers/ApplicationController.h"
#include "models/Project.h"
#include "dialogs/NewProjectDialog.h"
#include "dialogs/EditProjectDialog.h"

#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QScrollArea>
#include <QTextLayout>
#include <QTextOption>
#include <QMenu>
#include <QAction>
#include <QMessageBox>
#include <QContextMenuEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QLinearGradient>
#include <QRadialGradient>
#include <QGraphicsDropShadowEffect>
#include <QPropertyAnimation>
#include <QEasingCurve>
#include <QApplication>
#include <QStyle>
#include <QLayout>
#include <QHash>
#include <random>
#include <cmath>

// =============================================================================
//                              FlowLayout (Qt example, trimmed)
// =============================================================================
class FlowLayout : public QLayout
{
public:
    FlowLayout(QWidget* parent, int margin = 0, int hSpacing = -1, int vSpacing = -1)
        : QLayout(parent), m_hSpace(hSpacing), m_vSpace(vSpacing) {
        setContentsMargins(margin, margin, margin, margin);
    }
    ~FlowLayout() override { QLayoutItem* it; while ((it = takeAt(0))) delete it; }

    void addItem(QLayoutItem* item) override { itemList.append(item); }
    int count() const override { return itemList.size(); }
    QLayoutItem* itemAt(int i) const override { return itemList.value(i); }
    QLayoutItem* takeAt(int i) override { return (i >= 0 && i < itemList.size()) ? itemList.takeAt(i) : nullptr; }
    Qt::Orientations expandingDirections() const override { return {}; }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int w) const override { return doLayout(QRect(0,0,w,0), true); }
    void setGeometry(const QRect& r) override { QLayout::setGeometry(r); doLayout(r, false); }
    QSize sizeHint() const override { return minimumSize(); }
    QSize minimumSize() const override {
        QSize s;
        for (auto* it : itemList) s = s.expandedTo(it->minimumSize());
        const QMargins m = contentsMargins();
        return s + QSize(m.left()+m.right(), m.top()+m.bottom());
    }
private:
    int doLayout(const QRect& rect, bool testOnly) const {
        QMargins m = contentsMargins();
        QRect eff = rect.adjusted(m.left(), m.top(), -m.right(), -m.bottom());
        int hs = m_hSpace >= 0 ? m_hSpace : 16;
        int vs = m_vSpace >= 0 ? m_vSpace : 16;

        int y = eff.y();
        int i = 0;
        while (i < itemList.size()) {
            // ---- pass 1: gather items that fit on this row ----
            int rowW = 0;
            int rowH = 0;
            int j = i;
            for (; j < itemList.size(); ++j) {
                QSize sh = itemList[j]->sizeHint();
                int needed = (j == i) ? sh.width() : (rowW + hs + sh.width());
                if (j > i && needed > eff.width()) break;
                rowW = needed;
                rowH = qMax(rowH, sh.height());
            }
            // ---- pass 2: place them centered horizontally ----
            int xStart = eff.x() + qMax(0, (eff.width() - rowW) / 2);
            int x = xStart;
            for (int k = i; k < j; ++k) {
                QSize sh = itemList[k]->sizeHint();
                if (!testOnly) itemList[k]->setGeometry(QRect(QPoint(x, y), sh));
                x += sh.width() + hs;
            }
            y += rowH + vs;
            i = j;
        }
        return y - vs - rect.y() + m.bottom();
    }
    QList<QLayoutItem*> itemList;
    int m_hSpace, m_vSpace;
};

// =============================================================================
//                              Constants
// =============================================================================
static constexpr int  CARD_W       = 240;
static constexpr int  CARD_H       = 280;
static constexpr int  CARD_RADIUS  = 14;
static constexpr int  MAX_LIFT     = 6;     // pixels the card visually rises on hover
static constexpr int  WIDGET_PAD_B = MAX_LIFT; // bottom padding to host the lift travel + shadow

// =============================================================================
//                          ProjectSelectionView
// =============================================================================
ProjectSelectionView::ProjectSelectionView(ApplicationController* controller, QWidget* parent)
    : QWidget(parent), _controller(controller)
{
    setupUi();
    loadProjects();
}

ProjectSelectionView::~ProjectSelectionView() = default;

void ProjectSelectionView::setupUi()
{
    auto* main = new QVBoxLayout(this);
    main->setContentsMargins(24, 24, 24, 24);
    main->setSpacing(16);

    auto* title = new QLabel("Select a Project");
    title->setStyleSheet("font-size: 24px; font-weight: 600;");
    main->addWidget(title);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setStyleSheet("QScrollArea { background: transparent; }"
                          "QScrollArea > QWidget > QWidget { background: transparent; }");

    _flowContainer = new QWidget;
    _flowContainer->setObjectName("flowContainer");
    _flowContainer->setStyleSheet("#flowContainer { background: transparent; }");
    _flowLayout = new FlowLayout(_flowContainer, 0, 20, 20);

    _flowLayout->setContentsMargins(0, 10, 0, 36);

    auto* scrollContent = new QWidget;
    scrollContent->setStyleSheet("background: transparent;");
    auto* contentLayout = new QVBoxLayout(scrollContent);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(0);
    contentLayout->addStretch(1);
    contentLayout->addWidget(_flowContainer);
    contentLayout->addStretch(1);

    scroll->setWidget(scrollContent);
    main->addWidget(scroll, 1);

    _newProjectCard = new NewProjectCard(_flowContainer);
    connect(_newProjectCard, &NewProjectCard::clicked,
            this, &ProjectSelectionView::onNewProjectClicked);
}

void ProjectSelectionView::loadProjects()
{
    // wipe everything currently in the flow
    for (auto* card : _projectCards) {
        _flowLayout->removeWidget(card);
        card->deleteLater();
    }
    _projectCards.clear();
    _flowLayout->removeWidget(_newProjectCard);

    // "+ New Project" always first
    _flowLayout->addWidget(_newProjectCard);

    for (const auto& project : _controller->getProjects()) {
        auto* card = new ProjectCard(
            project->getId(), project->getName(), project->getDescription(),
            project->getStarCount(), project->getImagePath(), _flowContainer);

        connect(card, &ProjectCard::clicked,         this, &ProjectSelectionView::onProjectCardClicked);
        connect(card, &ProjectCard::editRequested,   this, &ProjectSelectionView::onProjectEdit);
        connect(card, &ProjectCard::deleteRequested, this, &ProjectSelectionView::onProjectDelete);

        _projectCards.append(card);
        _flowLayout->addWidget(card);
    }
    _flowContainer->updateGeometry();
}

void ProjectSelectionView::onProjectCardClicked(const QString& id) { emit projectSelected(id); }
void ProjectSelectionView::onNewProjectClicked() { createNewProject(); }

void ProjectSelectionView::createNewProject()
{
    NewProjectDialog dlg(this);
    if (dlg.exec() == QDialog::Accepted) {
        _controller->createProject(dlg.projectName(), dlg.projectDescription(), dlg.thumbnailPath());
        refreshProjects();
    }
}

void ProjectSelectionView::editProject(std::shared_ptr<Project> project)
{
    EditProjectDialog dlg(project, this);
    if (dlg.exec() == QDialog::Accepted) {
        dlg.applyChanges();
        _controller->updateProject(project);
        refreshProjects();
    }
}

void ProjectSelectionView::refreshProjects() { loadProjects(); }

void ProjectSelectionView::onProjectEdit(const QString& id)
{
    for (const auto& p : _controller->getProjects())
        if (p->getId() == id) { editProject(p); break; }
}

void ProjectSelectionView::onProjectDelete(const QString& id)
{
    for (const auto& p : _controller->getProjects()) {
        if (p->getId() != id) continue;
        QMessageBox box(this);
        box.setWindowTitle("Confirm Delete");
        box.setText(QString("You are about to delete \"%1\" containing %2 stars.")
                        .arg(p->getName()).arg(p->getStarCount()));
        box.setInformativeText("Are you sure?");
        box.setStandardButtons(QMessageBox::Cancel);
        auto* del = box.addButton("Delete", QMessageBox::DestructiveRole);
        del->setStyleSheet("QPushButton { background-color: #dc3545; color: white; }");
        box.setDefaultButton(QMessageBox::Cancel);
        if (box.exec() == QMessageBox::Cancel) return;
        if (box.clickedButton() == del) {
            _controller->deleteProject(id);
            refreshProjects();
        }
        break;
    }
}

// =============================================================================
//                                ProjectCard
// =============================================================================
ProjectCard::ProjectCard(const QString& id, const QString& name,
                         const QString& description, int starCount,
                         const QString& imagePath, QWidget* parent)
    : QWidget(parent)
    , _projectId(id), _name(name), _description(description)
    , _imagePath(imagePath), _starCount(starCount)
{
    setFixedSize(CARD_W, CARD_H + WIDGET_PAD_B);
    setCursor(Qt::PointingHandCursor);
    setAttribute(Qt::WA_Hover, true);

    if (!imagePath.isEmpty()) {
        QPixmap p(imagePath);
        if (!p.isNull()) { _imagePixmap = p; _hasImage = true; }
    }

    _shadow = new QGraphicsDropShadowEffect(this);
    _shadow->setBlurRadius(14);
    _shadow->setOffset(0, 4);
    _shadow->setColor(QColor(0, 0, 0, 90));
    setGraphicsEffect(_shadow);

    createContextMenu();
    rebuildBackground();
}

ProjectCard::~ProjectCard()
{
    delete _contextMenu;
}

void ProjectCard::createContextMenu()
{
    // No parent → menu doesn't inherit anything from the card's widget chain.
    // We'll clean it up manually in the destructor.
    _contextMenu = new QMenu();
    _contextMenu->setPalette(QApplication::palette());
    _contextMenu->setStyle(QApplication::style());

    auto* open = _contextMenu->addAction("Open Project");
    auto* edit = _contextMenu->addAction("Edit Project");
    _contextMenu->addSeparator();
    auto* del  = _contextMenu->addAction("Delete Project");

    connect(open, &QAction::triggered, this, [this]{ emit clicked(_projectId); });
    connect(edit, &QAction::triggered, this, [this]{ emit editRequested(_projectId); });
    connect(del,  &QAction::triggered, this, [this]{ emit deleteRequested(_projectId); });
}

void ProjectCard::contextMenuEvent(QContextMenuEvent* event)
{
    // Re-sync just before showing in case the application palette changed at runtime.
    _contextMenu->setPalette(QApplication::palette());
    _contextMenu->exec(event->globalPos());
}

void ProjectCard::resizeEvent(QResizeEvent* e) { QWidget::resizeEvent(e); rebuildBackground(); }

void ProjectCard::rebuildBackground()
{
    if (_hasImage) buildFrostedBackground();
    else           buildStarrySkyBackground();
}

void ProjectCard::buildFrostedBackground()
{
    QSize sz(CARD_W, CARD_H);
    _bgPixmap = QPixmap(sz);
    _bgPixmap.fill(Qt::transparent);

    // Downscale → upscale = soft "frosted glass / noise gradient" with image's palette.
    QPixmap tiny  = _imagePixmap.scaled(10, 10, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    QPixmap blown = tiny.scaled(sz, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

    QPainter p(&_bgPixmap);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.drawPixmap(0, 0, blown);

    // subtle darkening for legibility
    p.fillRect(_bgPixmap.rect(), QColor(0, 0, 0, 55));

    // vignette
    QRadialGradient vg(sz.width()/2.0, sz.height()/2.0,
                       std::max(sz.width(), sz.height()) * 0.75);
    vg.setColorAt(0.0, QColor(0,0,0,0));
    vg.setColorAt(1.0, QColor(0,0,0,110));
    p.fillRect(_bgPixmap.rect(), vg);
}

void ProjectCard::buildStarrySkyBackground()
{
    QSize sz(CARD_W, CARD_H);
    _bgPixmap = QPixmap(sz);
    _bgPixmap.fill(Qt::transparent);

    QPainter p(&_bgPixmap);
    p.setRenderHint(QPainter::Antialiasing);

    // Deterministic per project
    std::mt19937 rng(static_cast<uint32_t>(qHash(_projectId)));
    auto frand = [&](qreal a, qreal b) {
        return a + (b - a) * (rng() / qreal(std::mt19937::max()));
    };

    auto starColor = [](qreal t) -> QColor {
        const QColor red  (255, 165, 145);   
        const QColor white(255, 250, 240);
        const QColor blue (185, 205, 255);   
        auto lerp = [](int a, int b, qreal k){ return int(a + (b - a) * k); };
        qreal m = std::abs(t);
        const QColor& target = (t < 0) ? red : blue;
        return QColor(lerp(white.red(),   target.red(),   m),
                    lerp(white.green(), target.green(), m),
                    lerp(white.blue(),  target.blue(),  m));
    };

    // Deep-space gradient
    int hue = int(frand(200, 260));      // blueish-purple base
    QLinearGradient grad(0, 0, 0, sz.height());
    grad.setColorAt(0.0, QColor::fromHsv(hue,  140, 60));
    grad.setColorAt(0.6, QColor::fromHsv(hue,  160, 28));
    grad.setColorAt(1.0, QColor::fromHsv(hue,  180, 10));
    p.fillRect(_bgPixmap.rect(), grad);

    // a couple of soft "nebula" blobs
    int nNebula = 2 + int(frand(0, 3));
    for (int i = 0; i < nNebula; ++i) {
        QPointF c(frand(0, sz.width()), frand(0, sz.height()));
        qreal r = frand(60, 130);
        QRadialGradient ng(c, r);
        QColor nc = QColor::fromHsv(int(frand(180, 320)), 140, 180);
        nc.setAlpha(40);
        ng.setColorAt(0, nc);
        nc.setAlpha(0);
        ng.setColorAt(1, nc);
        p.setBrush(ng); p.setPen(Qt::NoPen);
        p.drawEllipse(c, r, r);
    }

    // Stars
    _stars.clear();
    int nStars = 50 + int(frand(0, 30));
    for (int i = 0; i < nStars; ++i) {
        Star s;
        s.pos = QPointF(frand(6, sz.width()-6), frand(6, sz.height()-6));

        // Only ~15% of stars are bright enough to bloom; the rest are quiet pinpoints.
        const bool isBright = frand(0.0, 1.0) < 0.15;
        if (isBright) {
            s.brightness = frand(0.85, 1.00);
            s.radius     = frand(1.10, 2.20);
        } else {
            s.brightness = frand(0.28, 0.55);
            s.radius     = frand(0.40, 1.00);
        }

        // Color temperature: very strong bias toward white. Red/blue extremes are rare.
        qreal t   = frand(-1.0, 1.0);
        qreal mag = std::pow(std::abs(t), 3.0);   // steep curve -> most stars near 0
        t = (t < 0 ? -mag : mag);
        s.color = starColor(t);

        _stars.append(s);
    }

    // Constellations: pick a few clusters, link them in a path
    _constellationLines.clear();
    int nConst = 2 + int(frand(0, 3));
    for (int c = 0; c < nConst; ++c) {
        QPointF anchor(frand(20, sz.width()-20), frand(20, sz.height()-20));
        // collect nearest N stars to the anchor
        QList<QPair<qreal,int>> dists;
        for (int i = 0; i < _stars.size(); ++i) {
            qreal dx = _stars[i].pos.x() - anchor.x();
            qreal dy = _stars[i].pos.y() - anchor.y();
            dists.append({std::sqrt(dx*dx + dy*dy), i});
        }
        std::sort(dists.begin(), dists.end(),
                  [](const QPair<qreal,int>& a, const QPair<qreal,int>& b){ return a.first < b.first; });

        int nNodes = 3 + int(frand(0, 4));
        nNodes = std::min(nNodes, int(dists.size()));
        QList<int> idx;
        for (int i = 0; i < nNodes; ++i) idx.append(dists[i].second);

        // boost brightness of constellation stars
        for (int i : idx) {
            _stars[i].brightness = std::min<qreal>(1.0, _stars[i].brightness + 0.3);
            _stars[i].radius     = std::max<qreal>(_stars[i].radius, 1.4);
        }

        for (int i = 0; i < idx.size() - 1; ++i)
            _constellationLines.append({idx[i], idx[i+1]});
    }

    // Draw constellation lines
    QPen linePen(QColor(200, 215, 255, 70));
    linePen.setWidthF(0.8);
    p.setPen(linePen);
    for (const auto& l : _constellationLines)
        p.drawLine(_stars[l.first].pos, _stars[l.second].pos);

    p.setPen(Qt::NoPen);
    for (const auto& s : _stars) {
        // Only "bright" stars get a bloom. The bloom uses a wide radius and many
        // gradient stops so the falloff approximates a Gaussian — no visible edge.
        if (s.brightness > 0.7) {
            const qreal R = s.radius * 11.0;
            QRadialGradient g(s.pos, R);

            auto stop = [&](qreal pos, qreal alpha){
                QColor c = s.color;
                c.setAlphaF(alpha * s.brightness);
                g.setColorAt(pos, c);
            };
            // Hot core, then smoothly decaying halo to zero at the edge.
            stop(0.00, 0.55);
            stop(0.08, 0.34);
            stop(0.18, 0.18);
            stop(0.32, 0.085);
            stop(0.50, 0.035);
            stop(0.72, 0.010);
            stop(0.90, 0.002);
            stop(1.00, 0.000);

            p.setBrush(g);
            p.drawEllipse(s.pos, R, R);
        }

        // Crisp core for every star.
        QColor core = s.color;
        core.setAlphaF(s.brightness);
        p.setBrush(core);
        p.drawEllipse(s.pos, s.radius, s.radius);
    }
}

void ProjectCard::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.setRenderHint(QPainter::TextAntialiasing);

    // The painted card occupies CARD_W x CARD_H within the widget; lift shifts it up.
    const qreal restY = WIDGET_PAD_B;
    const qreal yTop  = restY - _lift;
    QRectF cardRect(0, yTop, CARD_W, CARD_H);

    QPainterPath cardClip;
    cardClip.addRoundedRect(cardRect, CARD_RADIUS, CARD_RADIUS);

    // ------------------------------------------------------------------
    // Clipped section: background, optional image, bottom dark gradient,
    // and title/subtitle text. Single save/restore pair around the whole
    // clipped block to keep the state ledger balanced.
    // ------------------------------------------------------------------
    p.save();
    p.setClipPath(cardClip);

    // 1) background
    p.drawPixmap(cardRect.topLeft(), _bgPixmap);

    // 2) sharp image preview (if any)
    if (_hasImage) {
        const int margin   = 14;
        const int textArea = 92;
        QRectF imgArea(cardRect.left() + margin,
                       cardRect.top()  + margin,
                       cardRect.width()  - margin * 2,
                       cardRect.height() - textArea - margin * 2);

        QPixmap scaled = _imagePixmap.scaled(imgArea.size().toSize(),
                                             Qt::KeepAspectRatio,
                                             Qt::SmoothTransformation);
        QRectF dst(imgArea.left() + (imgArea.width()  - scaled.width())  / 2.0,
                   imgArea.top()  + (imgArea.height() - scaled.height()) / 2.0,
                   scaled.width(), scaled.height());

        QPainterPath imgClip;
        imgClip.addRoundedRect(dst, 8, 8);

        p.save();
        p.setClipPath(imgClip);
        p.drawPixmap(dst.topLeft(), scaled);
        p.restore();
    }

    // 3) bottom gradient for text legibility
    const int gradH = 120;
    QLinearGradient tg(0, cardRect.bottom() - gradH, 0, cardRect.bottom());
    tg.setColorAt(0.0, QColor(0, 0, 0,   0));
    tg.setColorAt(1.0, QColor(0, 0, 0, 190));
    p.fillRect(QRectF(cardRect.left(), cardRect.bottom() - gradH,
                      cardRect.width(), gradH), tg);

    // 4) title (word-wrapped, max 2 lines, ellided) + subtitle
    QFont titleFont = font();
    titleFont.setPointSizeF(titleFont.pointSizeF() + 4.5);
    titleFont.setBold(true);

    QFont subFont = font();
    QFontMetricsF tfm(titleFont);
    QFontMetricsF sfm(subFont);

    const qreal padX     = 16.0;
    const qreal padBot   = 14.0;
    const qreal titleW   = cardRect.width() - padX * 2;
    const qreal lineH    = tfm.lineSpacing();
    const int   maxLines = 2;

    auto layoutTitle = [&](const QString& src, QVector<QTextLine>& out) {
        QTextLayout* tl = new QTextLayout(src, titleFont);
        QTextOption opt;
        opt.setWrapMode(QTextOption::WordWrap);
        tl->setTextOption(opt);
        tl->beginLayout();
        while (out.size() < maxLines) {
            QTextLine line = tl->createLine();
            if (!line.isValid()) break;
            line.setLineWidth(titleW);
            out.append(line);
        }
        tl->endLayout();
        return tl;
    };

    QVector<QTextLine> linesOut;
    QTextLayout* tl = layoutTitle(_name, linesOut);

    int consumed = 0;
    for (const auto& l : linesOut) consumed += l.textLength();
    if (consumed < _name.length()) {
        delete tl;
        int headLen = 0;
        for (int i = 0; i < linesOut.size() - 1; ++i) headLen += linesOut[i].textLength();
        QString head       = _name.left(headLen);
        QString tail       = _name.mid(headLen);
        QString elidedTail = tfm.elidedText(tail, Qt::ElideRight, titleW);
        linesOut.clear();
        tl = layoutTitle(head + elidedTail, linesOut);
    }

    const qreal subBaselineY = cardRect.bottom() - padBot - sfm.descent();
    const qreal subTop       = subBaselineY - sfm.ascent();
    const qreal titleBottom  = subTop - 4.0;
    const qreal titleTop     = titleBottom - linesOut.size() * lineH;

    p.setPen(QColor(255, 255, 255));
    for (int i = 0; i < linesOut.size(); ++i)
        linesOut[i].draw(&p, QPointF(cardRect.left() + padX, titleTop + i * lineH));
    delete tl;

    p.setFont(subFont);
    p.setPen(QColor(225, 230, 240, 215));
    QRectF subRect(cardRect.left() + padX, subTop, titleW, sfm.height());
    p.drawText(subRect, Qt::AlignLeft | Qt::AlignVCenter,
               QString("%1 star%2").arg(_starCount).arg(_starCount == 1 ? "" : "s"));

    p.restore();   // <-- pairs with the p.save() above; ledger now balanced

    // ------------------------------------------------------------------
    // Unclipped section: crisp 1px inner border on top of everything.
    // ------------------------------------------------------------------
    QPen border(QColor(255, 255, 255, 35));
    border.setWidthF(1.0);
    p.setPen(border);
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(cardRect.adjusted(0.5, 0.5, -0.5, -0.5),
                      CARD_RADIUS, CARD_RADIUS);
}

void ProjectCard::animateHover(bool hovered)
{
    if (_liftAnim)  { _liftAnim->stop();  _liftAnim->deleteLater();  _liftAnim  = nullptr; }
    if (_blurAnim)  { _blurAnim->stop();  _blurAnim->deleteLater();  _blurAnim  = nullptr; }
    if (_offsetAnim){ _offsetAnim->stop();_offsetAnim->deleteLater();_offsetAnim= nullptr; }

    _liftAnim = new QPropertyAnimation(this, "lift");
    _liftAnim->setDuration(180);
    _liftAnim->setEasingCurve(QEasingCurve::OutCubic);
    _liftAnim->setEndValue(hovered ? qreal(MAX_LIFT) : qreal(0));
    _liftAnim->start(QAbstractAnimation::DeleteWhenStopped);
    connect(_liftAnim, &QObject::destroyed, this, [this]{ _liftAnim = nullptr; });

    _blurAnim = new QPropertyAnimation(_shadow, "blurRadius");
    _blurAnim->setDuration(180);
    _blurAnim->setEasingCurve(QEasingCurve::OutCubic);
    _blurAnim->setEndValue(hovered ? 32.0 : 14.0);
    _blurAnim->start(QAbstractAnimation::DeleteWhenStopped);
    connect(_blurAnim, &QObject::destroyed, this, [this]{ _blurAnim = nullptr; });

    _offsetAnim = new QPropertyAnimation(_shadow, "yOffset");
    _offsetAnim->setDuration(180);
    _offsetAnim->setEasingCurve(QEasingCurve::OutCubic);
    _offsetAnim->setEndValue(hovered ? 12.0 : 4.0);
    _offsetAnim->start(QAbstractAnimation::DeleteWhenStopped);
    connect(_offsetAnim, &QObject::destroyed, this, [this]{ _offsetAnim = nullptr; });
}

void ProjectCard::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton)
        emit clicked(_projectId);
    QWidget::mousePressEvent(event);
}


void ProjectCard::enterEvent(QEnterEvent* event) { Q_UNUSED(event); animateHover(true); }
void ProjectCard::leaveEvent(QEvent* event)      { Q_UNUSED(event); animateHover(false); }

// =============================================================================
//                              NewProjectCard
// =============================================================================
NewProjectCard::NewProjectCard(QWidget* parent) : QWidget(parent)
{
    setFixedSize(CARD_W, CARD_H + WIDGET_PAD_B);
    setCursor(Qt::PointingHandCursor);
    setAttribute(Qt::WA_Hover, true);
}

void NewProjectCard::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);

    const qreal yTop = WIDGET_PAD_B;
    QRectF r(0, yTop, CARD_W, CARD_H);

    // Pull colors from the active palette so this works in light AND dark themes.
    QColor base   = palette().color(QPalette::WindowText);
    QColor accent = palette().color(QPalette::Highlight);

    QColor fill, stroke, text;
    if (_hovered) {
        fill = accent;   fill.setAlpha(55);
        stroke = accent;
        text   = accent.lightness() > 160 ? accent.darker(130) : accent;
    } else {
        fill = base;     fill.setAlpha(14);
        stroke = base;   stroke.setAlpha(150);
        text   = base;   text.setAlpha(190);
    }

    QPainterPath path;
    path.addRoundedRect(r, CARD_RADIUS, CARD_RADIUS);
    p.fillPath(path, fill);

    QPen pen(stroke);
    pen.setWidthF(1.6);
    pen.setStyle(Qt::DashLine);
    pen.setDashPattern({6, 4});
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(r.adjusted(1, 1, -1, -1), CARD_RADIUS, CARD_RADIUS);

    // Big "+"
    p.setPen(QPen(text, 2.2, Qt::SolidLine, Qt::RoundCap));
    QPointF c = r.center() - QPointF(0, 14);
    const qreal s = 22;
    p.drawLine(QPointF(c.x() - s, c.y()), QPointF(c.x() + s, c.y()));
    p.drawLine(QPointF(c.x(), c.y() - s), QPointF(c.x(), c.y() + s));

    QFont f = font();
    f.setPointSizeF(f.pointSizeF() + 0.5);
    f.setBold(true);
    p.setFont(f);
    p.setPen(text);
    QRectF label(r.left(), c.y() + s + 14, r.width(), 24);
    p.drawText(label, Qt::AlignHCenter | Qt::AlignVCenter, "Create New Project");
}

void NewProjectCard::mousePressEvent(QMouseEvent* e)
{
    if (e->button() == Qt::LeftButton) emit clicked();
    QWidget::mousePressEvent(e);
}

void NewProjectCard::enterEvent(QEnterEvent*) { _hovered = true;  update(); }
void NewProjectCard::leaveEvent(QEvent*)      { _hovered = false; update(); }