#!/usr/bin/env python3
"""
Reorganize ASTRA's src/ tree from a layer-first layout (models/utils/views/dialogs)
into a domain-first layout (core/catalog/lightcurve/rv/spectra/...), where every
domain folder holds its Core-only code flat and all QtWidgets code under <domain>/ui/.

What it does, in order:
  1. builds the old -> new path mapping and asserts every file under src/ is covered
  2. rewrites #include "..." in src/, tests/ and tools/ to root-anchored new paths
     (resolution happens against the OLD tree, so this must run before the moves)
  3. rewrites src/ paths in CMakeLists.txt, the build scripts and helper scripts
  4. git mv's every file to its new home
  5. runs post-checks (no "../" includes, no QtWidgets in Core files, CMake coverage)

It never commits. Run with --dry-run first.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

# ─────────────────────────────────────────────────────────────────────────────
# Mapping
# ─────────────────────────────────────────────────────────────────────────────

# "olddir/Stem" -> "newdir".  Expanded to whichever of .h/.cpp exist.
MOVES: dict[str, str] = {}


def _add(newdir: str, olddir: str, *stems: str) -> None:
    for stem in stems:
        key = f"{olddir}/{stem}"
        assert key not in MOVES, f"duplicate mapping for {key}"
        MOVES[key] = newdir


# ── app: application shell, settings, logging ────────────────────────────────
_add("app", "utils", "AppPaths", "AppSettings", "Logger", "PerfTimer", "DataStore")
_add("app/ui", "controllers", "ApplicationController")
_add("app/ui", "utils",
     "ThemeManager", "UpdateManager", "BackgroundTaskManager", "PlotPresetStore",
     "UiIcons", "UiStyle", "WindowSizing")
_add("app/ui", "views",
     "MainWindow", "ProjectView", "ProjectSelectionView", "StarDetailView",
     "StarFilterWidget", "InstrumentConfigView", "ColumnConfigDialog")
_add("app/ui", "dialogs",
     "SettingsDialog", "FirstRunDialog", "WhatsNewDialog", "NewProjectDialog",
     "EditProjectDialog", "ExportTableDialog")
_add("app/ui", "views/tools", "ProjectPlotDialog", "LcquerySetupDialog")
_add("app/ui/panels", "views/panels",
     "DetailPanel", "DetailPanelFactory", "PanelUtils", "SummaryPanel")

# ── ui/widgets: domain-free reusable widgets ─────────────────────────────────
_add("ui/widgets", "utils", "CheckBoxDragger", "CheckStateDragger", "WheelGuard")
_add("ui/widgets", "views", "BooleanColumnDelegate")
_add("ui/widgets", "views/widgets",
     "FlowLayout", "ResponsiveGridLayout", "ElidedLabel", "ShimmerWidget", "CopyToast",
     "PreciseDoubleSpinBox", "GridSelectorWidget", "QuantityLabel", "QuantityRenderer",
     "QuantityDelegate", "SquarePlotFrame", "PlotKeyNavigator")

# ── core: entities, time, quantities ─────────────────────────────────────────
_add("core", "models",
     "Time", "BarycentricCorrection", "Quantity", "AsymmetricErrors", "Star",
     "Project", "Instrument", "InstrumentMode", "ColumnPreset")
_add("core", "utils", "QuantityFormat", "FilterExpression")

# ── catalog: external catalogues, cross-identification, matching ─────────────
_add("catalog", "utils",
     "CdsTapClient", "CatalogQueryWorkers", "CrossRefResolver", "StarMatching",
     "StarSearchQuery")
_add("catalog/ui", "dialogs", "AddStarDialog", "ReidentifyStarDialog")
_add("catalog/ui", "views/tools", "CMDDialog")

# ── lightcurve ───────────────────────────────────────────────────────────────
_add("lightcurve", "models", "Photometry")
_add("lightcurve", "utils",
     "LCFitPhysics", "LCBinning", "ClaretTables", "ClaretFilter", "TessSectors",
     "LightcurveFetcher", "LightcurveFetchService", "LCFitRunner", "LcqueryEnvironment")
_add("lightcurve/ui", "views/panels", "LCPanel")
_add("lightcurve/ui", "dialogs", "LCFitDialog", "ImportLightcurve", "LightcurveCredentialPrompts")
_add("lightcurve/ui", "views/tools", "LightcurveFetchDialog", "LightcurveFetchSessionsDialog")
_add("lightcurve/ui", "views/widgets", "LCModelPreview")

# ── rv: radial velocities and period searches ────────────────────────────────
_add("rv", "models", "RadialVelocity", "PeriodogramRecord")
_add("rv", "fitting", "RVMCMC", "RVErrorMC", "RVDetectability", "Periodogram")
_add("rv/ui", "views/panels", "RVPanel", "PeriodogramPanel")
_add("rv/ui", "views/tools",
     "RVAddFitDialog", "RVAddPointDialog", "RVInspectorDialog", "RVMCMCResultsDialog",
     "RVDetectabilityDialog", "RVImportPointsDialog")

# ── spectra ──────────────────────────────────────────────────────────────────
_add("spectra", "models", "Spectrum")
_add("spectra", "utils",
     "SpectrumReader", "SpectrumCoadder", "SpectrumFetchService", "matchSpectraToInstrument")
_add("spectra/ui", "views/panels", "SpectraPanel")
_add("spectra/ui", "views/tools",
     "AddSpectraDialog", "ArchiveFetchWidget", "CoAddWidget", "SpectrumFetchSessionsDialog")

# ── fitting: spectral fitting backends ───────────────────────────────────────
_add("fitting", "utils", "IsisEnvironment", "SystematicErrors")
_add("fitting", "models", "ElementAbundances")
_add("fitting/ui", "views/tools",
     "SpectraFitDialog", "FitSetupWidget", "FitProgressDialog", "InteractiveIsisDialog")
_add("fitting/ui", "views/widgets", "FitComponentsWidget", "IsisMacroPanel", "FitPreviewOverlay")

# ── sed ──────────────────────────────────────────────────────────────────────
_add("sed", "utils", "ExtractSED", "FilterWavelength", "SedFitEnvironment")
_add("sed/ui", "views/tools", "SEDFitDialog")

# ── massfit ──────────────────────────────────────────────────────────────────
_add("massfit", "models", "MassFitPlan")
_add("massfit", "utils", "MassFitService")
_add("massfit/ui", "views/tools", "MassFitManagerDialog", "MassFitPlanDialog", "MassFitRuleEditor")

# ── kinematics ───────────────────────────────────────────────────────────────
_add("kinematics/ui", "views/tools", "GalacticOrbitDialog")

# ── observing ────────────────────────────────────────────────────────────────
_add("observing", "utils", "ObservabilityCalculator")
_add("observing/ui", "utils", "CoastlineData")
_add("observing/ui", "views/tools", "ObservabilityDialog")

# ── remote ───────────────────────────────────────────────────────────────────
_add("remote", "models", "RemoteHost")
_add("remote/ui", "remote", "AskPass", "RemoteSelfTest")
_add("remote/ui", "views/tools", "RemoteFitsDialog")
_add("remote/ui", "dialogs", "RemoteHostsSettingsPage")
_add("remote/ui", "views/widgets", "AnsiTerminalWidget", "TerminalView")

# ── io ───────────────────────────────────────────────────────────────────────
_add("io/ui", "io", "StarShare")

# Whole-directory moves, applied to files not named in MOVES.
DIR_MOVES: dict[str, str] = {
    "importWizard": "importwizard",
    "utils/spectrafetch": "spectra/fetch",
}

# Directories whose remaining files keep their home.
KEEP_DIRS = {"", "db", "kinematics", "plotting", "io", "fitting", "remote"}

SOURCE_SUFFIXES = {".cpp", ".h", ".hpp", ".cc"}

# Files outside CMakeLists.txt that carry src/ paths.
TEXT_TARGETS = [
    "CMakeLists.txt",
    "build-appimage.sh",
    "build-macos.sh",
    "build-macos-isis.sh",
    "build-windows.sh",
    "scripts/generate_prompt.py",
    "scripts/astra_data.py",
    "scripts/fix_sdss_time_convention.py",
    "tools/generate_kinematic_contours.cpp",
    "WINDOWS-HANDOFF.md",
    "README.md",
]

INCLUDE_RE = re.compile(r'^(\s*#\s*include\s*)(["<])([^">\n]+)([">])(.*)$')

# Qt classes that mean "this is UI code" for the post-check.
GUI_TOKENS = (
    "QtWidgets", "QtGui", "QWidget", "QApplication", "QDialog", "QLabel", "QStatusBar",
    "QMessageBox", "QFileDialog", "QInputDialog", "QProgressDialog", "QPainter",
    "QColor", "QPixmap", "QImage", "QPolygonF", "QIcon", "QFont", "QDesktopServices",
)

RESET, BOLD, DIM = "\033[0m", "\033[1m", "\033[2m"
RED, GREEN, YELLOW, CYAN = "\033[31m", "\033[32m", "\033[33m", "\033[36m"


def ok(msg: str) -> None:
    print(f"{GREEN}OK{RESET}   {msg}")


def info(msg: str) -> None:
    print(f"{CYAN}..{RESET}   {msg}")


def warn(msg: str) -> None:
    print(f"{YELLOW}WARN{RESET} {msg}")


def fail(msg: str) -> None:
    print(f"{RED}FAIL{RESET} {msg}")


# ─────────────────────────────────────────────────────────────────────────────
# Mapping construction
# ─────────────────────────────────────────────────────────────────────────────

def build_mapping(src: Path) -> dict[str, str]:
    """src-relative old path -> src-relative new path, for every source file."""
    mapping: dict[str, str] = {}
    uncovered: list[str] = []

    for path in sorted(src.rglob("*")):
        if not path.is_file() or path.suffix not in SOURCE_SUFFIXES:
            continue
        rel = path.relative_to(src).as_posix()
        olddir = str(Path(rel).parent).replace(".", "") if Path(rel).parent == Path(".") else Path(rel).parent.as_posix()
        stem = Path(rel).stem
        name = Path(rel).name

        key = f"{olddir}/{stem}"
        if key in MOVES:
            newdir = MOVES[key]
        elif olddir in DIR_MOVES:
            newdir = DIR_MOVES[olddir]
        elif olddir in KEEP_DIRS:
            newdir = olddir
        else:
            uncovered.append(rel)
            continue

        mapping[rel] = f"{newdir}/{name}" if newdir else name

    if uncovered:
        fail("files not covered by the mapping:")
        for rel in uncovered:
            print(f"       {rel}")
        sys.exit(1)

    seen: dict[str, str] = {}
    for old, new in mapping.items():
        if new in seen:
            fail(f"two files would land on {new}: {seen[new]} and {old}")
            sys.exit(1)
        seen[new] = old

    return mapping


# ─────────────────────────────────────────────────────────────────────────────
# Include rewriting
# ─────────────────────────────────────────────────────────────────────────────

def resolve_include(inc: str, includer_olddir: str, mapping: dict[str, str],
                    by_basename: dict[str, list[str]]) -> str | None:
    """Resolve a quoted include to its src-relative OLD path, or None."""
    inc = inc.replace("\\", "/")

    # (a) relative to the including file's old directory (covers bare and "../")
    base = Path(includer_olddir) if includer_olddir else Path(".")
    cand = os.path.normpath((base / inc).as_posix())
    if cand in mapping:
        return cand

    # (b) already root-anchored
    cand = os.path.normpath(inc)
    if cand in mapping:
        return cand

    # (c) unique basename (covers the bare "qcustomplot.h" and includes from tests/)
    hits = by_basename.get(Path(inc).name, [])
    if len(hits) == 1 and Path(inc).name == inc:
        return hits[0]

    return None


def rewrite_includes(root: Path, mapping: dict[str, str], dry: bool) -> tuple[int, list[str]]:
    src = root / "src"
    by_basename: dict[str, list[str]] = {}
    for old in mapping:
        by_basename.setdefault(Path(old).name, []).append(old)

    changed = 0
    unresolved: set[str] = set()

    files: list[Path] = []
    for sub in ("src", "tests", "tools"):
        d = root / sub
        if d.is_dir():
            files += [p for p in sorted(d.rglob("*")) if p.suffix in SOURCE_SUFFIXES]
    # qcustomplot is vendored third-party code and is compiled with only its own
    # directory on the include path; never touch its includes.
    files = [p for p in files if "plotting" not in p.parts]

    for path in files:
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError as e:
            warn(f"cannot read {path}: {e}")
            continue

        # Directory the includer sits in, expressed relative to src/ (old tree).
        if src in path.parents:
            includer_olddir = path.relative_to(src).parent.as_posix()
            if includer_olddir == ".":
                includer_olddir = ""
        else:
            includer_olddir = ""

        out, dirty = [], False
        for line in text.splitlines(keepends=True):
            m = INCLUDE_RE.match(line.rstrip("\n"))
            if not m:
                out.append(line)
                continue
            prefix, open_q, inc, close_q, suffix = m.groups()
            # Angle-bracket includes are resolved on the include path only, so a
            # project header written that way is always root-anchored.  Quoted
            # includes also resolve against the including file's own directory.
            if open_q == "<":
                old = os.path.normpath(inc.replace("\\", "/"))
                old = old if old in mapping else None
            else:
                old = resolve_include(inc, includer_olddir, mapping, by_basename)
            if old is None:
                if open_q == '"':
                    unresolved.add(inc)
                out.append(line)
                continue
            new_inc = mapping[old]
            if new_inc == inc and open_q == '"':
                out.append(line)
                continue
            nl = "\n" if line.endswith("\n") else ""
            out.append(f"{prefix}\"{new_inc}\"{suffix}{nl}")
            dirty = True

        if dirty:
            changed += 1
            if not dry:
                path.write_text("".join(out), encoding="utf-8")

    return changed, sorted(unresolved)


# ─────────────────────────────────────────────────────────────────────────────
# Path rewriting in CMake / scripts / docs
# ─────────────────────────────────────────────────────────────────────────────

def rewrite_text_paths(root: Path, mapping: dict[str, str], dry: bool) -> list[str]:
    targets = [root / t for t in TEXT_TARGETS if (root / t).is_file()]
    docs = root / "docs"
    if docs.is_dir():
        targets += sorted(docs.rglob("*.md"))

    # File paths first (longest old path first), then bare directory renames.
    file_subs = sorted(mapping.items(), key=lambda kv: len(kv[0]), reverse=True)
    # Only directories that moved wholesale can be rewritten blindly.  Folders
    # that were split across domains (views/tools, dialogs, ...) have no single
    # successor, so surviving mentions of those are reported, not guessed at.
    dir_subs = [
        ("src/controllers/", "src/app/ui/"),
        ("src/importWizard/", "src/importwizard/"),
        ("src/utils/spectrafetch/", "src/spectra/fetch/"),
    ]
    split_dirs = ("src/views/tools/", "src/views/panels/", "src/views/widgets/",
                  "src/views/", "src/dialogs/", "src/models/", "src/utils/")

    touched, leftovers = [], []
    for path in targets:
        text = original = path.read_text(encoding="utf-8", errors="replace")
        for old, new in file_subs:
            if old != new:
                text = text.replace(f"src/{old}", f"src/{new}")
        for old_dir, new_dir in dir_subs:
            text = text.replace(old_dir, new_dir)
        for i, line in enumerate(text.splitlines(), 1):
            for d in split_dirs:
                if d in line:
                    leftovers.append(f"{path.relative_to(root)}:{i}: {line.strip()[:90]}")
                    break
        if text != original:
            touched.append(str(path.relative_to(root)))
            if not dry:
                path.write_text(text, encoding="utf-8")

    if leftovers:
        warn(f"{len(leftovers)} mentions of a split directory need a human eye:")
        for l in leftovers[:25]:
            print(f"       {l}")
    return touched


# ─────────────────────────────────────────────────────────────────────────────
# The moves
# ─────────────────────────────────────────────────────────────────────────────

def git_mv(root: Path, mapping: dict[str, str], dry: bool) -> None:
    src = root / "src"
    moves = [(o, n) for o, n in sorted(mapping.items()) if o != n]
    info(f"{len(moves)} files to move, {len(mapping) - len(moves)} staying put")
    if dry:
        for old, new in moves:
            print(f"       {DIM}{old}{RESET} -> {CYAN}{new}{RESET}")
        return

    for old, new in moves:
        dest = src / new
        dest.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(["git", "mv", f"src/{old}", f"src/{new}"],
                       cwd=root, check=True)

    # Drop directories the moves emptied out.
    for d in sorted((p for p in src.rglob("*") if p.is_dir()),
                    key=lambda p: len(p.parts), reverse=True):
        if not any(d.iterdir()):
            d.rmdir()


# ─────────────────────────────────────────────────────────────────────────────
# Post-checks
# ─────────────────────────────────────────────────────────────────────────────

def is_ui(rel: str) -> bool:
    parts = Path(rel).parts
    return "ui" in parts or parts[0] == "importwizard" or rel == "main.cpp" or parts[0] == "plotting"


def post_check(root: Path, mapping: dict[str, str]) -> int:
    src = root / "src"
    problems = 0

    # 1. no relative includes left
    rel_includes = []
    for sub in ("src", "tests", "tools"):
        d = root / sub
        if not d.is_dir():
            continue
        for path in d.rglob("*"):
            if path.suffix not in SOURCE_SUFFIXES or "plotting" in path.parts:
                continue
            for i, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
                if re.match(r'\s*#\s*include\s*"\.\.?/', line):
                    rel_includes.append(f"{path.relative_to(root)}:{i}")
    if rel_includes:
        fail(f"{len(rel_includes)} relative includes remain")
        for r in rel_includes[:20]:
            print(f"       {r}")
        problems += 1
    else:
        ok("no relative includes remain")

    # 2. no QtWidgets/QtGui in Core files
    #
    # Known exception: three Core services hold an ApplicationController* purely to
    # reach settings()/databaseManager()/getCurrentProject().  Their headers only
    # forward-declare it, so astra_core compiles; the symbols resolve against the
    # executable.  A test that links one of these objects would fail to link until
    # the controller dependency is injected instead.  Tracked as a follow-up.
    KNOWN_UI_BACKREFS = {
        "lightcurve/LightcurveFetchService.cpp": "app/ui/ApplicationController.h",
        "massfit/MassFitService.cpp": "app/ui/ApplicationController.h",
        "spectra/SpectrumFetchService.cpp": "app/ui/ApplicationController.h",
    }
    violators = []
    for new in sorted(mapping.values()):
        if is_ui(new):
            continue
        path = src / new
        if not path.exists():
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        for line in text.splitlines():
            m = re.match(r'\s*#\s*include\s*[<"]([^>"]+)[>"]', line)
            if not m:
                continue
            inc = m.group(1)
            if KNOWN_UI_BACKREFS.get(new) == inc:
                continue
            if any(tok == inc or inc.endswith(f"/{tok}") for tok in GUI_TOKENS):
                violators.append(f"{new}: {inc}")
            elif re.search(r'(^|/)ui/', inc):
                violators.append(f"{new}: {inc}")
    if violators:
        fail(f"{len(violators)} Core files include UI headers")
        for v in violators[:30]:
            print(f"       {v}")
        problems += 1
    else:
        ok(f"no Core file includes a QtWidgets/QtGui or ui/ header "
           f"({len(KNOWN_UI_BACKREFS)} documented exceptions)")

    # 3. every .cpp is listed in CMakeLists.txt
    cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8", errors="replace")
    missing = [n for n in sorted(mapping.values())
               if n.endswith(".cpp") and f"src/{n}" not in cmake]
    if missing:
        fail(f"{len(missing)} .cpp files are not listed in CMakeLists.txt")
        for m in missing[:20]:
            print(f"       {m}")
        problems += 1
    else:
        ok("every .cpp under src/ is listed in CMakeLists.txt")

    return problems


def emit_source_lists(root: Path, mapping: dict[str, str]) -> None:
    """Print the astra_core / ASTRA source and header lists for the CMake rewrite."""
    core_src, core_hdr, ui_src, ui_hdr = [], [], [], []
    for new in sorted(mapping.values()):
        if new == "main.cpp" or new.startswith("plotting/"):
            continue
        bucket_src, bucket_hdr = (ui_src, ui_hdr) if is_ui(new) else (core_src, core_hdr)
        (bucket_src if new.endswith(".cpp") else bucket_hdr).append(new)

    out = root / "build" / "astra_source_lists.cmake"
    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open("w", encoding="utf-8") as f:
        for name, items in (("ASTRA_CORE_SOURCES", core_src), ("ASTRA_CORE_HEADERS", core_hdr),
                            ("ASTRA_UI_SOURCES", ui_src), ("ASTRA_UI_HEADERS", ui_hdr)):
            f.write(f"set({name}\n")
            last_dir = None
            for i in items:
                d = str(Path(i).parent)
                if d != last_dir:
                    f.write(f"\n    # {d}\n")
                    last_dir = d
                f.write(f"    src/{i}\n")
            f.write(")\n\n")
    info(f"source lists written to {out.relative_to(root)} "
         f"(core: {len(core_src)} cpp / {len(core_hdr)} h, ui: {len(ui_src)} cpp / {len(ui_hdr)} h)")


# ─────────────────────────────────────────────────────────────────────────────

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dry-run", action="store_true", help="show what would change")
    ap.add_argument("--force", action="store_true", help="run even with a dirty worktree")
    ap.add_argument("--check-only", action="store_true", help="only run the post-checks")
    args = ap.parse_args()

    root = Path(__file__).resolve().parent.parent
    src = root / "src"
    if not (src / "main.cpp").exists() and not (src / "main.cpp").is_file():
        fail(f"{src} does not look like ASTRA's source tree")
        return 1

    if args.check_only:
        # The tree has already been reorganized, so the old->new mapping no longer
        # applies; check the layout that is actually on disk.
        current = {p.relative_to(src).as_posix(): p.relative_to(src).as_posix()
                   for p in sorted(src.rglob("*"))
                   if p.is_file() and p.suffix in SOURCE_SUFFIXES}
        ok(f"checking {len(current)} files in the current tree")
        return 1 if post_check(root, current) else 0

    mapping = build_mapping(src)
    ok(f"mapping covers {len(mapping)} files")

    if not args.dry_run and not args.force:
        dirty = subprocess.run(["git", "status", "--porcelain"], cwd=root,
                               capture_output=True, text=True).stdout.strip()
        if dirty:
            fail("worktree is dirty; commit or stash first (or pass --force)")
            return 1

    print(f"\n{BOLD}1. include rewriting{RESET}")
    changed, unresolved = rewrite_includes(root, mapping, args.dry_run)
    ok(f"{changed} files with rewritten includes")
    if unresolved:
        info(f"{len(unresolved)} includes left untouched (not part of src/): "
             + ", ".join(unresolved[:10]) + ("..." if len(unresolved) > 10 else ""))

    print(f"\n{BOLD}2. path rewriting in CMake and scripts{RESET}")
    touched = rewrite_text_paths(root, mapping, args.dry_run)
    ok(f"{len(touched)} files updated: {', '.join(touched)}")

    print(f"\n{BOLD}3. moves{RESET}")
    git_mv(root, mapping, args.dry_run)

    if args.dry_run:
        print(f"\n{DIM}dry run: nothing was written{RESET}")
        return 0

    print(f"\n{BOLD}4. post-checks{RESET}")
    problems = post_check(root, mapping)
    emit_source_lists(root, mapping)

    print()
    if problems:
        warn("post-checks reported problems; review before building")
    else:
        ok("reorganization complete; nothing was committed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
