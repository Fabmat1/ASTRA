# Troubleshooting & FAQ

## The AppImage won't start

- **Missing FUSE**: install `libfuse2` (Debian/Ubuntu) or `fuse2` (Arch), or
  run with `--appimage-extract-and-run`:
  ```bash
  ./astra-0.6.0-x86_64.AppImage --appimage-extract-and-run
  ```
- **Missing system libraries**: make sure the
  [runtime dependencies](installation.md#linux-appimage-recommended) are
  installed. Running the AppImage from a terminal shows which library is
  missing.

## macOS says the app is damaged or can't be opened

The app is not notarized with Apple. Right-click → **Open**, allow it under
**System Settings → Privacy & Security**, or clear the quarantine flag:

```bash
xattr -dr com.apple.quarantine /Applications/ASTRA.app
```

## Plots don't appear / plotting errors

ASTRA calls out to Python and gnuplot for some plots. Verify that `python3`,
`python3-numpy`, and `gnuplot` are installed and on your `PATH`.

## Where is my data stored?

Your catalog lives in a local SQLite database in your OS application-data
directory; spectra and other data files are stored next to it. See
[Database & Files](reference/database.md) for exact locations and backup
advice.

<!-- TODO: grow this page from real user questions / GitHub issues. -->

## Still stuck?

Open an issue on the
[GitHub issue tracker](https://github.com/Fabmat1/ASTRA/issues). Please
include your OS, ASTRA version, and terminal output if there is any.
