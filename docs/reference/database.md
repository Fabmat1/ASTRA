# Database & Files

## Where ASTRA stores your data

ASTRA keeps everything in a single application-data directory, created
automatically on first launch (you are never asked for a location):

| Platform | Location |
|---|---|
| Linux | `~/.local/share/ASTRA/ASTRA/` |
| macOS | `~/Library/Application Support/ASTRA/` |

Inside that directory:

| Path | Contents |
|---|---|
| `astra.db` | The SQLite database - all projects, stars, catalog values, RV points, fit results, and light curves |
| `media/` | Project thumbnails |
| `logs/` | Application logs |

Spectra and other bulk data files referenced by the database are also stored
under the application-data directory.

!!! tip "Backups"
    Backing up the application-data directory backs up everything. The
    database is a plain SQLite file, so you can also inspect it with any
    SQLite client. Close ASTRA first, and treat the schema as internal:
    it changes between versions (ASTRA migrates it automatically on upgrade).

## Reading the data from Python

The [`scripts/astra_data.py`](scripts.md) module gives read-only Python
access to the database and its `.asd` data files. It is the recommended way to get
your data into notebooks and custom analysis scripts.

## Sharing data

For moving stars between machines or colleagues, don't copy database files -
use `.astra` share packages
(see [Catalog Management](../workflows/catalog-management.md#sharing-stars-with-colleagues)),
which bundle a star with its spectra, RVs, fits, and light curves and merge
cleanly into any project.

<!-- TODO: document the .asd file format and the .astra package format at a
     high level, and confirm the exact app-data paths on each platform. -->
