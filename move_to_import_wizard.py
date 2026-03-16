#!/usr/bin/env python3
"""
move_to_import_wizard.py

Safely moves C++ "Pages" + wizard files from src/utils/ into src/importWizard/,
rewrites all #include directives across the entire project, and updates CMakeLists.txt.

Usage:
    python move_to_import_wizard.py [PROJECT_ROOT] [options]

Fish shell example:
    python move_to_import_wizard.py ~/Projects/ASTRA
    python move_to_import_wizard.py ~/Projects/ASTRA --dry-run
    python move_to_import_wizard.py ~/Projects/ASTRA --no-backup
    python move_to_import_wizard.py ~/Projects/ASTRA --extra GeneralUtils.cpp GeneralUtils.h
"""

import argparse
import os
import re
import shutil
import sys
from datetime import datetime
from pathlib import Path

# ─── ANSI colours (fish-style) ───────────────────────────────────────────────
RESET  = "\033[0m"
BOLD   = "\033[1m"
RED    = "\033[31m"
GREEN  = "\033[32m"
YELLOW = "\033[33m"
CYAN   = "\033[36m"
DIM    = "\033[2m"

def ok(msg):    print(f"{GREEN}✔{RESET} {msg}")
def info(msg):  print(f"{CYAN}•{RESET} {msg}")
def warn(msg):  print(f"{YELLOW}⚠{RESET}  {msg}")
def err(msg):   print(f"{RED}✘{RESET} {msg}", file=sys.stderr)
def head(msg):  print(f"\n{BOLD}{msg}{RESET}")
def dim(msg):   print(f"{DIM}{msg}{RESET}")

# ─── Files to migrate (basenames only, without extension) ────────────────────
DEFAULT_TARGETS = [
    "GeneralImportPage",
    "RadialVelocityImportPage",
    "RVPhotometryPages",
    "SEDImportPage",
    "SpectraImportPage",
    "SpectralFitImportPage",
    "StarImportWizard",
    "ImportStagingArea",
]

EXTENSIONS = [".cpp", ".h", ".hpp", ".cxx", ".cc"]

# File extensions to scan for #include rewrites
SCAN_EXTENSIONS = {".cpp", ".h", ".hpp", ".cxx", ".cc", ".c", ".txt"}

# ─── Helpers ─────────────────────────────────────────────────────────────────

def find_project_root(start: Path) -> Path:
    """Walk upward to find the directory containing CMakeLists.txt + src/."""
    candidate = start.resolve()
    for _ in range(10):
        if (candidate / "CMakeLists.txt").exists() and (candidate / "src").exists():
            return candidate
        candidate = candidate.parent
    return start.resolve()


def collect_files_to_move(utils_dir: Path, targets: list[str]) -> list[tuple[Path, str]]:
    """
    Return list of (absolute_path, basename) for every target file found.
    Warns about targets with no matching files.
    """
    found = []
    for stem in targets:
        matched = False
        for ext in EXTENSIONS:
            p = utils_dir / (stem + ext)
            if p.exists():
                found.append((p, p.name))
                matched = True
        if not matched:
            warn(f"No files found for target '{stem}' in {utils_dir} — skipping")
    return found


def compute_include_rewrites(
    files_to_move: list[tuple[Path, str]],
    utils_dir: Path,
    dest_dir: Path,
    src_dir: Path,
) -> dict[str, dict[str, str]]:
    """
    For every file that will be scanned, compute a mapping of:
        old_include_string -> new_include_string

    We handle every sane relative path variant a developer might have written,
    relative to the file doing the including.

    Returns: { scanning_file_abs_str: { old: new, ... } }
    We actually return a global map of old->new here and apply per-file later.
    """
    basenames = {p.name for p, _ in files_to_move}

    # Build a lookup: basename -> (old_abs, new_abs)
    moves: dict[str, tuple[Path, Path]] = {}
    for old_abs, name in files_to_move:
        new_abs = dest_dir / name
        moves[name] = (old_abs, new_abs)

    return moves  # callers resolve relative paths per file


def rewrite_includes_in_file(
    file_path: Path,
    moves: dict[str, tuple[Path, Path]],
    dry_run: bool,
) -> list[str]:
    """
    Rewrite #include directives in `file_path` that reference any moved file.
    Returns list of human-readable change descriptions.
    Handles:
      - quoted includes: #include "foo.h", #include "../utils/foo.h", etc.
      - angle-bracket includes: #include <foo.h> (rare for project files, still handled)
      - extra whitespace between #include and the path
      - Windows-style backslashes in paths
      - multi-include lines won't appear in valid C++ but we won't corrupt them
    """
    try:
        original = file_path.read_text(encoding="utf-8", errors="replace")
    except OSError as e:
        warn(f"  Could not read {file_path}: {e}")
        return []

    lines = original.splitlines(keepends=True)
    new_lines = []
    changes = []

    # Regex: capture the quote char (" or </>), and the full include path
    include_re = re.compile(
        r'^(\s*#\s*include\s*)'   # #include with optional spaces
        r'(["\<])'                 # opening quote or angle bracket
        r'([^">\n]+)'              # the path itself
        r'(["\>])'                 # closing quote or angle bracket
        r'(.*)',                   # rest of line (comments etc.)
        re.ASCII,
    )

    for lineno, line in enumerate(lines, 1):
        m = include_re.match(line)
        if not m:
            new_lines.append(line)
            continue

        prefix, open_q, inc_path, close_q, suffix = m.groups()

        # Normalise backslashes
        inc_path_norm = inc_path.replace("\\", "/")
        inc_basename = Path(inc_path_norm).name

        if inc_basename not in moves:
            new_lines.append(line)
            continue

        old_abs, new_abs = moves[inc_basename]

        # Compute the new relative path from this file's directory to new_abs
        try:
            new_rel = new_abs.relative_to(file_path.parent)
        except ValueError:
            # new_abs is not under file_path.parent — compute a ../.. style path
            new_rel = Path(os.path.relpath(new_abs, file_path.parent))

        # Normalise to forward slashes
        new_inc_path = str(new_rel).replace("\\", "/")

        # Preserve the quote style the developer used
        new_line = f"{prefix}{open_q}{new_inc_path}{close_q}{suffix}"
        if not new_line.endswith("\n") and line.endswith("\n"):
            new_line += "\n"

        if new_line != line:
            changes.append(
                f"  {file_path.name}:{lineno}  "
                f"{DIM}{inc_path}{RESET} → {CYAN}{new_inc_path}{RESET}"
            )
        new_lines.append(new_line)

    if changes and not dry_run:
        file_path.write_text("".join(new_lines), encoding="utf-8")

    return changes


def rewrite_cmake(
    cmake_path: Path,
    moves: dict[str, tuple[Path, Path]],
    src_dir: Path,
    dry_run: bool,
) -> list[str]:
    """
    Update file paths in CMakeLists.txt files.
    Handles both explicit listings and common glob patterns.
    Replaces e.g. `utils/GeneralImportPage.cpp` with `importWizard/GeneralImportPage.cpp`.
    Also warns about glob patterns that may implicitly cover the moved files.
    """
    try:
        original = cmake_path.read_text(encoding="utf-8", errors="replace")
    except OSError as e:
        warn(f"  Could not read {cmake_path}: {e}")
        return []

    changes = []
    new_content = original

    for name, (old_abs, new_abs) in moves.items():
        # Compute old and new paths relative to cmake_path's directory
        cmake_dir = cmake_path.parent
        try:
            old_rel = str(old_abs.relative_to(cmake_dir)).replace("\\", "/")
            new_rel = str(new_abs.relative_to(cmake_dir)).replace("\\", "/")
        except ValueError:
            old_rel = str(os.path.relpath(old_abs, cmake_dir)).replace("\\", "/")
            new_rel = str(os.path.relpath(new_abs, cmake_dir)).replace("\\", "/")

        if old_rel in new_content:
            new_content = new_content.replace(old_rel, new_rel)
            changes.append(f"  {cmake_path.name}: {DIM}{old_rel}{RESET} → {CYAN}{new_rel}{RESET}")

    # Warn about globs that may need manual attention
    glob_re = re.compile(r'GLOB[_A-Z]*\s+[A-Z_]+\s+"?([^"\s)]+\*[^"\s)]*)"?', re.IGNORECASE)
    for m in glob_re.finditer(original):
        pattern = m.group(1)
        if "utils" in pattern or "src" in pattern:
            warn(
                f"  {cmake_path.name} contains a GLOB pattern '{pattern}' — "
                "verify it still covers all desired files after the move"
            )

    if changes and not dry_run:
        cmake_path.write_text(new_content, encoding="utf-8")

    return changes


def backup_project(src_dir: Path, backup_root: Path) -> Path:
    """Copy src/ into a timestamped backup directory."""
    ts = datetime.now().strftime("%Y%m%dT%H%M%S")
    backup_path = backup_root / f"astra_backup_{ts}"
    shutil.copytree(src_dir, backup_path)
    return backup_path


def scan_all_source_files(project_root: Path) -> list[Path]:
    """Return every source/cmake file in the project worth scanning."""
    result = []
    for ext in SCAN_EXTENSIONS:
        result.extend(project_root.rglob(f"*{ext}"))
    # Exclude hidden dirs, build dirs, and the backup dir
    excluded = {"build", ".git", "__pycache__", "backup"}
    return [
        p for p in result
        if not any(part in excluded for part in p.parts)
        and p.is_file()
    ]


# ─── Main ────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="Move ASTRA import wizard files from utils/ to importWizard/ safely.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument(
        "project_root",
        nargs="?",
        default=".",
        help="Path to the ASTRA project root (default: current directory)",
    )
    parser.add_argument(
        "--dry-run", "-n",
        action="store_true",
        help="Show what would happen without making any changes",
    )
    parser.add_argument(
        "--no-backup",
        action="store_true",
        help="Skip creating a backup of src/ before making changes",
    )
    parser.add_argument(
        "--extra", "-e",
        nargs="+",
        metavar="FILE",
        help="Additional bare filenames (with or without extension) to include in the move",
    )
    parser.add_argument(
        "--src-subdir",
        default="src",
        metavar="DIR",
        help="Name of the source subdirectory (default: src)",
    )
    parser.add_argument(
        "--from-dir",
        default="utils",
        metavar="DIR",
        help="Subdirectory inside src/ to move files FROM (default: utils)",
    )
    parser.add_argument(
        "--to-dir",
        default="importWizard",
        metavar="DIR",
        help="Subdirectory inside src/ to move files TO (default: importWizard)",
    )

    args = parser.parse_args()

    # ── Resolve paths ──────────────────────────────────────────────────────
    given_root = Path(args.project_root).expanduser().resolve()
    project_root = find_project_root(given_root)

    if project_root != given_root:
        info(f"Project root auto-detected at: {project_root}")

    src_dir   = project_root / args.src_subdir
    utils_dir = src_dir / args.from_dir
    dest_dir  = src_dir / args.to_dir

    head("ASTRA — import wizard file migration")
    if args.dry_run:
        print(f"  {YELLOW}DRY RUN — no files will be changed{RESET}\n")

    info(f"Project root : {project_root}")
    info(f"Source dir   : {src_dir}")
    info(f"Moving FROM  : {utils_dir}")
    info(f"Moving TO    : {dest_dir}")

    # ── Validate ───────────────────────────────────────────────────────────
    if not src_dir.exists():
        err(f"src/ directory not found at {src_dir}")
        sys.exit(1)
    if not utils_dir.exists():
        err(f"Source directory not found: {utils_dir}")
        sys.exit(1)

    # ── Build target list ──────────────────────────────────────────────────
    targets = list(DEFAULT_TARGETS)
    if args.extra:
        for f in args.extra:
            stem = Path(f).stem
            if stem not in targets:
                targets.append(stem)
                info(f"Added extra target: {stem}")

    # ── Collect files ──────────────────────────────────────────────────────
    head("Scanning for files to move")
    files_to_move = collect_files_to_move(utils_dir, targets)

    if not files_to_move:
        err("No matching files found. Nothing to do.")
        sys.exit(1)

    for p, name in files_to_move:
        ok(f"Found: {p.relative_to(project_root)}")

    # Check for destination collisions
    head("Checking for conflicts")
    conflicts = []
    if dest_dir.exists():
        for _, name in files_to_move:
            target = dest_dir / name
            if target.exists():
                conflicts.append(target)
                warn(f"Already exists at destination: {target.relative_to(project_root)}")
    else:
        ok(f"Destination directory does not yet exist — will create {dest_dir.relative_to(project_root)}")

    if conflicts:
        err("Destination conflicts found. Aborting to avoid data loss.")
        err("Remove or rename the conflicting files and try again.")
        sys.exit(1)
    else:
        ok("No conflicts detected")

    # ── Backup ────────────────────────────────────────────────────────────
    if not args.dry_run and not args.no_backup:
        head("Creating backup")
        backup_dir = project_root / "backup"
        backup_dir.mkdir(exist_ok=True)
        backup_path = backup_project(src_dir, backup_dir)
        ok(f"Backup created: {backup_path.relative_to(project_root)}")
    elif args.dry_run:
        info("(Backup skipped in dry-run mode)")
    else:
        warn("Backup skipped (--no-backup)")

    # ── Compute rewrite map ────────────────────────────────────────────────
    moves = compute_include_rewrites(files_to_move, utils_dir, dest_dir, src_dir)

    # ── Rewrite includes BEFORE moving files ──────────────────────────────
    # (so relative paths are computed correctly from original locations)
    head("Rewriting #include directives")
    all_files = scan_all_source_files(project_root)
    total_changes = []

    for f in sorted(all_files):
        if f.suffix == ".txt":
            continue  # CMake handled separately below
        file_changes = rewrite_includes_in_file(f, moves, dry_run=args.dry_run)
        if file_changes:
            info(f"{f.relative_to(project_root)}")
            for c in file_changes:
                print(c)
            total_changes.extend(file_changes)

    if not total_changes:
        ok("No #include rewrites needed")

    # ── Also rewrite includes inside the files being moved ─────────────────
    # (their includes of *each other* stay the same if they land in the same dir,
    #  but their includes of sibling utils files need ../utils/ prepended)
    head("Checking cross-includes inside moved files")
    moved_basenames = {p.name for p, _ in files_to_move}

    for old_abs, name in files_to_move:
        # Temporarily treat the file as if it's already at the destination
        # by recomputing relative paths from dest_dir
        try:
            content = old_abs.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue

        include_re = re.compile(
            r'^(\s*#\s*include\s*)(["\<])([^">\n]+)(["\>])(.*)',
            re.ASCII | re.MULTILINE,
        )

        new_content = content
        file_changes = []

        for m in include_re.finditer(content):
            prefix, open_q, inc_path, close_q, suffix = m.groups()
            inc_path_norm = inc_path.replace("\\", "/")
            inc_basename = Path(inc_path_norm).name

            # If this include references a non-moved utils file (i.e. staying in utils)
            # we need to adjust the path since we're moving one level sideways
            referenced = utils_dir / inc_basename
            if referenced.exists() and inc_basename not in moved_basenames:
                # This file stays in utils; we need ../utils/basename
                new_rel = Path("../") / args.from_dir / inc_basename
                new_inc = str(new_rel).replace("\\", "/")
                old_include = f"{prefix}{open_q}{inc_path}{close_q}{suffix}"
                new_include = f"{prefix}{open_q}{new_inc}{close_q}{suffix}"
                new_content = new_content.replace(old_include, new_include, 1)
                file_changes.append(
                    f"  {name}: {DIM}{inc_path}{RESET} → {CYAN}{new_inc}{RESET}"
                )

        if file_changes:
            info(f"{name} (internal cross-include fix)")
            for c in file_changes:
                print(c)
            if not args.dry_run:
                old_abs.write_text(new_content, encoding="utf-8")
            total_changes.extend(file_changes)

    # ── Update CMakeLists.txt files ────────────────────────────────────────
    head("Updating CMakeLists.txt")
    cmake_files = list(project_root.rglob("CMakeLists.txt"))
    cmake_files = [f for f in cmake_files if "backup" not in str(f)]

    cmake_changes = []
    for cmake in sorted(cmake_files):
        changes = rewrite_cmake(cmake, moves, src_dir, dry_run=args.dry_run)
        if changes:
            info(f"{cmake.relative_to(project_root)}")
            for c in changes:
                print(c)
            cmake_changes.extend(changes)

    if not cmake_changes:
        ok("No CMakeLists.txt changes needed (check manually if using GLOB)")

    # ── Move the files ────────────────────────────────────────────────────
    head("Moving files")
    if not args.dry_run:
        dest_dir.mkdir(parents=True, exist_ok=True)

    for old_abs, name in files_to_move:
        new_abs = dest_dir / name
        if args.dry_run:
            dim(f"  [dry] mv {old_abs.relative_to(project_root)} → {new_abs.relative_to(project_root)}")
        else:
            shutil.move(str(old_abs), str(new_abs))
            ok(f"Moved: {old_abs.relative_to(project_root)} → {new_abs.relative_to(project_root)}")

    # ── Summary ───────────────────────────────────────────────────────────
    head("Summary")
    print(f"  Files moved           : {len(files_to_move)}")
    print(f"  #include rewrites     : {len(total_changes)}")
    print(f"  CMakeLists.txt edits  : {len(cmake_changes)}")

    if args.dry_run:
        print(f"\n  {YELLOW}Dry run complete — no changes were made.{RESET}")
        print(f"  Re-run without --dry-run to apply.")
    else:
        print(f"\n  {GREEN}{BOLD}Done!{RESET}")
        if not args.no_backup:
            print(f"  Backup is at: {backup_path.relative_to(project_root)}")
        print(f"\n  {CYAN}Next steps:{RESET}")
        print(f"    1. cmake --build build/ 2>&1 | grep -i error")
        print(f"    2. If clangd shows stale errors, restart it: {DIM}:LspRestart{RESET}")
        print(f"    3. If all good, delete the backup: {DIM}rm -rf backup/{RESET}")


if __name__ == "__main__":
    main()