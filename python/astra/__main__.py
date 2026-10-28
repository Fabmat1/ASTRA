"""``python -m astra info [ROOT]``: the data root, its projects and row counts."""

import argparse
import sys

from . import Database, is_running, paths


def info(root=None):
    db = Database(root, pandas=False)
    print(f"Data root:  {db.root}")
    print(f"Database:   {db.path}")
    live = is_running(db.root)
    print(f"Live ASTRA: {'running, socket ' + paths.socket_path(db.root) if live else 'not running'}")
    projects = db.projects()
    print(f"\nProjects ({len(projects)}):")
    width = max([len(p["name"] or "") for p in projects] + [4])
    for p in projects:
        print(f"  {p['name'] or '':{width}}  {p['n_stars']:6d} stars  {p['id']}")
    print("\nRows:")
    for table, n in db.counts().items():
        print(f"  {table:16s} {n}")
    db.close()


def main(argv=None):
    ap = argparse.ArgumentParser(prog="python -m astra",
                                 description="ASTRA data access from Python.")
    sub = ap.add_subparsers(dest="cmd")
    p = sub.add_parser("info", help="data root, projects and counts")
    p.add_argument("root", nargs="?", help="data root (default: $ASTRA_DATA_DIR or the "
                                           "platform default)")
    args = ap.parse_args(argv)
    if args.cmd != "info":
        ap.print_help()
        return 1
    try:
        info(args.root)
    except FileNotFoundError as e:
        print(e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
