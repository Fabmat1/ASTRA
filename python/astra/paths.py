"""Where ASTRA keeps its data, and the name of its local MCP socket."""

import hashlib
import os
import re
import sys


def default_root():
    """The ASTRA data root: $ASTRA_DATA_DIR, else the platform default.

    The platform default is Qt's AppDataLocation for organisation and
    application "ASTRA": ~/.local/share/ASTRA/ASTRA on Linux (respecting
    $XDG_DATA_HOME), ~/Library/Application Support/ASTRA/ASTRA on macOS and
    %APPDATA%\\ASTRA\\ASTRA on Windows. A build configured with its own
    ASTRA_DATA_DIR uses that path instead, which this package cannot know:
    pass the root explicitly or set $ASTRA_DATA_DIR then.
    """
    env = os.environ.get("ASTRA_DATA_DIR")
    if env:
        return os.path.expanduser(env)
    home = os.path.expanduser("~")
    if sys.platform == "darwin":
        return os.path.join(home, "Library", "Application Support", "ASTRA", "ASTRA")
    if os.name == "nt":
        base = os.environ.get("APPDATA") or os.path.join(home, "AppData", "Roaming")
        return os.path.join(base, "ASTRA", "ASTRA")
    base = os.environ.get("XDG_DATA_HOME") or os.path.join(home, ".local", "share")
    return os.path.join(base, "ASTRA", "ASTRA")


def resolve_root(root=None):
    """Absolute data root for `root` (None: default_root())."""
    return os.path.abspath(os.path.expanduser(root or default_root()))


def _user_name():
    u = os.environ.get("USER") or os.environ.get("USERNAME") or ""
    u = re.sub(r"[^A-Za-z0-9_.-]", "", u)
    return u or "user"


def server_name(root=None):
    """The QLocalServer name ASTRA listens on for this data root.

    Mirrors LocalSocketServer::serverName(): astra-mcp-<user>-<first ten hex
    digits of sha1(canonical root path)>.
    """
    root = resolve_root(root)
    canon = os.path.realpath(root) if os.path.isdir(root) else root
    if os.name == "nt":
        canon = canon.replace("\\", "/")         # Qt paths use forward slashes
    h = hashlib.sha1(canon.encode("utf-8")).hexdigest()[:10]
    return f"astra-mcp-{_user_name()}-{h}"


def socket_path(root=None):
    """Filesystem path (Unix) or pipe name (Windows) of ASTRA's MCP socket."""
    name = server_name(root)
    if os.name == "nt":
        return "\\\\.\\pipe\\" + name
    tmp = os.environ.get("TMPDIR", "").rstrip("/") or "/tmp"   # QDir::tempPath()
    return os.path.join(tmp, name)


def config_dir():
    """ASTRA's per-user config folder (QStandardPaths::AppConfigLocation)."""
    if os.name == "nt":
        base = os.environ.get("LOCALAPPDATA") or os.path.expanduser("~/AppData/Local")
    elif sys.platform == "darwin":
        base = os.path.expanduser("~/Library/Preferences")
    else:
        base = os.environ.get("XDG_CONFIG_HOME") or os.path.expanduser("~/.config")
    return os.path.join(base, "ASTRA", "ASTRA")


def token_path(root=None):
    """The file holding the running ASTRA's socket token.

    It lives in ASTRA's config folder, named after the socket; versions
    before 0.10.0 kept it in <root>/ai/mcp.token, which is still tried.
    """
    current = os.path.join(config_dir(), server_name(root) + ".token")
    if os.path.exists(current):
        return current
    legacy = os.path.join(resolve_root(root), "ai", "mcp.token")
    return legacy if os.path.exists(legacy) else current
