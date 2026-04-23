#include "IsisMacroPanel.h"

#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QToolTip>
#include <QVBoxLayout>

IsisMacroPanel::IsisMacroPanel(QWidget* parent) : QWidget(parent)
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto* host = new QWidget;
    _layout = new QVBoxLayout(host);
    _layout->setContentsMargins(4, 4, 4, 4);
    _layout->setSpacing(4);

    // ── Inspection ────────────────────────────────────────────
    addGroup("Inspection");
    addMacro({ "cheatsheet",  "Show the built-in cheatsheet", {}, {"cheatsheet;"} });
    addMacro({ "help hotkeys","Hotkey reference",             {}, {"help hotkeys;"} });
    addMacro({ "list_par",    "List all parameters",          {}, {"list_par;"} });
    addMacro({ "list_free",   "List free parameters",         {}, {"list_free;"} });
    addMacro({ "evaluate (ec)", "Evaluate current model",     {}, {"ec;"} });

    // ── Parameters ────────────────────────────────────────────
    addGroup("Parameters");
    addMacro({ "set_par", "Set parameter value",
               { {"name","*teff",120}, {"value","23000",80} },
               { "set_par(\"{0}\", {1});" } });
    addMacro({ "freeze pattern", "Freeze params matching a glob",
               { {"pattern","*",100} },
               { "freeze(\"{0}\");" } });
    addMacro({ "thaw pattern",   "Thaw params matching a glob",
               { {"pattern","*teff",100} },
               { "thaw(\"{0}\");" } });
    addMacro({ "tie_all()",  "Tie shared params across datasets", {}, {"tie_all();"} });
    addMacro({ "freeze_ignored()", "Freeze anchors on ignored regions",
               {}, {"freeze_ignored();"} });

    // ── Fitting ──────────────────────────────────────────────
    addGroup("Fitting");
    addMacro({ "fit_cont",   "Fit continuum only",  {}, {"fit_cont;"} });
    addMacro({ "fit_vrad",   "Fit radial velocity", {}, {"fit_vrad;"} });
    addMacro({ "fit_quick",  "Quick fit of free params", {}, {"fit_quick;"} });
    addMacro({ "save_quick", "Save quick snapshot",      {}, {"save_quick;"} });

    addMacro({ "stage → teff", "freeze(*) · thaw(*teff) · fit_quick",
               {}, {"freeze(\"*\");", "thaw(\"*teff\");", "fit_quick;"} });
    addMacro({ "stage → +logg +HE", "thaw(*logg) · thaw(*HE) · fit_quick",
               {}, {"thaw(\"*logg\");", "thaw(\"*HE\");", "fit_quick;"} });
    addMacro({ "stage → continuum", "thaw(c*y*) · fit_quick",
               {}, {"thaw(\"c*y*\");", "fit_quick;"} });

    // ── Tellurics ────────────────────────────────────────────
    addGroup("Tellurics");
    addMacro({ "fit_telluric",          "Fit telluric component",             {}, {"fit_telluric;"} });
    addMacro({ "remove_telluric",       "Fit then remove telluric lines",     {}, {"remove_telluric;"} });
    addMacro({ "remove_telluric_model", "Only remove telluric model",         {}, {"remove_telluric_model;"} });

    // ── Hotkeys / outliers ───────────────────────────────────
    addGroup("Hotkeys / outliers");
    addMacro({ "hotkeys",         "Enable hotkey-driven inspection window",
               {}, {"hotkeys;"} });
    addMacro({ "hotkeys(; ll)",   "Hotkeys with active line list",
               {}, {"hotkeys(; linelist=ll);"} });
    addMacro({ "add_systematic_errors", "Add systematic errors + re-fit",
               {}, {"add_systematic_errors;", "fit_quick;", "fit_quick;"} });

    // ── Uncertainties ────────────────────────────────────────
    addGroup("Uncertainties");
    addMacro({ "rescale_errors", "Rescale errors so χ²_red = 1",
               { {"chi_thres","10.",60} },
               { "rescale_errors(; chi_thres={0});" } });
    addMacro({ "compute_uncertainties",       "Use covariances",
               {}, {"compute_uncertainties();"} });
    addMacro({ "compute_uncertainties (conf)","Use confidence intervals",
               {}, {"compute_uncertainties(; conf);"} });

    // ── Per-spectrum / individual ────────────────────────────
    addGroup("Per-spectrum");
    addMacro({ "link_ignore",      "Link ignore regions across spectra", {}, {"link_ignore;"} });
    addMacro({ "reload_ignore",    "Reload ignore regions",              {}, {"reload_ignore;"} });
    addMacro({ "fit_individual sequence",
               "vrad · individual · sys err · individual · unc.",
               {},
               { "fit_vrad;", "fit_individual;", "add_systematic_errors;",
                 "fit_individual;", "uncertainties_individual;" } });

    // ── Line lists / masks ───────────────────────────────────
    addGroup("Line lists / masks");
    addMacro({ "read_fort12", "Read synspec line list",
               { {"file","fort.12",120}, {"thres","10.",60} },
               { "ll = read_fort12(\"{0}\", {1});" } });
    addMacro({ "create_specific_linelists", "Build per-line mask list",
               { {"abs_thres","3.",60} },
               { "ll = create_specific_linelists(ll; absent_chi_threshold={0});" } });
    addMacro({ "apply_mask", "Apply mask with hydrogen weighting",
               { {"hydrogen","0.6",60} },
               { "apply_mask(ll; hydrogen={0});" } });
    addMacro({ "apply_mask (save)", "Apply and save mask",
               { {"hydrogen","0.6",60} },
               { "apply_mask(ll; save, hydrogen={0});" } });
    addMacro({ "continuum_correction", "Interactive local continuum",
               {},
               { "cc = continuum_correction(ll; interactive);",
                 "save_local_continuum(cc);" } });
    addMacro({ "ignore_thres", "Ignore outliers past thresholds",
               { {"id","1",40}, {"lo","-3.",60}, {"hi","5.",60} },
               { "ignore_thres({0}, {1}, {2});" } });

    // ── UV continuum ─────────────────────────────────────────
    addGroup("UV continuum");
    addMacro({ "freeze cont nodes", "freeze(\"c*y*\")",
               {}, {"freeze(\"c*y*\");"} });
    addMacro({ "fit_cont_UV", "Iterative UV continuum fit",
               { {"low","-4",60}, {"high","5.",60} },
               { "fit_cont_UV({0}, {1});" } });

    // ── Output ───────────────────────────────────────────────
    addGroup("Output");
    addMacro({ "write_spec (ascii)", "Write spectra as ASCII",
               {}, {"write_spec(; ascii);"} });
    addMacro({ "plot_spec", "Render PDF of current fit",
               { {"xrange","1",40}, {"width","35",40},
                 {"ymax","1.06",60} },
               { "plot_spec(; xrange={0}, width={1}, ymax={2}, ll=ll);" } });
    addMacro({ "derive_vrad()", "Derive vrad for binary (needs q set)",
               {}, {"derive_vrad();"} });

    _layout->addStretch();
    scroll->setWidget(host);
    outer->addWidget(scroll);
}

void IsisMacroPanel::addGroup(const QString& title)
{
    auto* lbl = new QLabel(QString("<b>%1</b>").arg(title));
    lbl->setContentsMargins(2, 6, 2, 2);
    _layout->addWidget(lbl);
}

void IsisMacroPanel::addMacro(const IsisMacroDef& def)
{
    auto* row = new QWidget;
    auto* hl = new QHBoxLayout(row);
    hl->setContentsMargins(0, 0, 0, 0);
    hl->setSpacing(4);

    auto* btn = new QPushButton(def.label);
    btn->setAutoDefault(false);
    btn->setDefault(false);
    btn->setToolTip(def.tooltip.isEmpty()
        ? def.commandTemplates.join(" · ")
        : def.tooltip + "\n\n" + def.commandTemplates.join(" · "));
    btn->setMinimumWidth(140);
    hl->addWidget(btn);

    QVector<QLineEdit*> edits;
    for (const auto& p : def.params) {
        auto* e = new QLineEdit(p.defaultValue);
        e->setFixedWidth(p.widthPx);
        e->setToolTip(p.label);
        edits.append(e);
        hl->addWidget(e);
    }
    hl->addStretch();

    connect(btn, &QPushButton::clicked, this, [this, def, edits]{
        QStringList commands;
        for (const QString& tmpl : def.commandTemplates) {
            QString cmd = tmpl;
            for (int i = 0; i < edits.size(); ++i)
                cmd.replace(QString("{%1}").arg(i),
                            edits[i]->text().trimmed());
            commands << cmd;
        }
        emit runCommands(commands);
    });

    _layout->addWidget(row);
}