"""
A client for the running ASTRA: its local MCP socket.

Reading needs no ASTRA (``astra.open``). Changing data does: go through the
running application so its permission checks, change log and undo
(``revert_change``) apply and the open window stays in sync::

    with astra.connect() as live:
        live.tools()                                  # what ASTRA offers
        live.call("find_stars", query="PG 1618+562")
        live.call("set_star_fields", star_ids=[sid], fields={"spec_class": "sdB"})

Protocol (src/ai/LocalSocketServer.cpp, src/ai/McpSession.cpp): a Unix
socket (Windows: named pipe) named by ``astra.paths.socket_path(root)``; the
client first sends ``{"astra_auth": <token from the config folder>}``, then
speaks JSON-RPC 2.0 (MCP), one JSON message per line.

Actions that need approval wait for the user in the ASTRA window; a call can
therefore block until they answer (or time out).
"""

import json
import os
import socket

from .paths import resolve_root, socket_path, token_path

PROTOCOL_VERSION = "2025-06-18"


class AstraError(RuntimeError):
    """ASTRA refused or failed a request."""


class ToolError(AstraError):
    """A tool call came back with isError; the message is ASTRA's text."""


class _PipeStream:
    """Windows named pipe as a byte stream."""

    def __init__(self, name):
        self.f = open(name, "r+b", buffering=0)

    def send(self, data):
        self.f.write(data)

    def recv(self, n):
        return self.f.read(n)

    def close(self):
        self.f.close()


class Client:
    """A session with the running ASTRA. Create it with ``astra.connect()``.

    Attributes: ``mode`` ("window" or "headless"), ``server_info``,
    ``instructions`` (ASTRA's conventions text), ``notifications`` (the
    server notifications received so far).
    """

    def __init__(self, root=None, timeout=300.0, client_name="astra-python"):
        self.root = resolve_root(root)
        self.timeout = timeout
        self.notifications = []
        self._buf = b""
        self._next_id = 1
        self._tools = None
        try:
            with open(token_path(self.root), encoding="latin-1") as f:
                token = f.read().strip()
        except OSError:
            raise AstraError(
                f"ASTRA is not serving agents for {self.root} (no token at {token_path(self.root)}). Start ASTRA "
                "with this data root and enable the AI server, or pass the right root.") from None
        addr = socket_path(self.root)
        try:
            if os.name == "nt":
                self._sock = _PipeStream(addr)
            else:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.settimeout(timeout)
                s.connect(addr)
                self._sock = s
        except OSError as e:
            raise AstraError(f"Cannot reach ASTRA at {addr}: {e}") from None
        self._send({"astra_auth": token})
        reply = self._read()
        if reply.get("astra_auth") != "ok":
            self.close()
            raise AstraError(f"ASTRA refused the connection: {reply}")
        self.mode = reply.get("mode")
        init = self.request("initialize", {
            "protocolVersion": PROTOCOL_VERSION,
            "capabilities": {},
            "clientInfo": {"name": client_name, "version": "1"},
        })
        self.server_info = init.get("serverInfo", {})
        self.instructions = init.get("instructions", "")
        self.protocol_version = init.get("protocolVersion")
        self._send({"jsonrpc": "2.0", "method": "notifications/initialized"})

    # ── transport ────────────────────────────────────────────────────────

    def _send(self, msg):
        data = json.dumps(msg, separators=(",", ":")).encode("utf-8") + b"\n"
        if os.name == "nt":
            self._sock.send(data)
        else:
            self._sock.sendall(data)

    def _read(self):
        while b"\n" not in self._buf:
            chunk = self._sock.recv(1 << 16)
            if not chunk:
                raise AstraError("ASTRA closed the connection")
            self._buf += chunk
        line, self._buf = self._buf.split(b"\n", 1)
        line = line.strip()
        return json.loads(line) if line else self._read()

    def _handle_server_request(self, msg):
        method, mid = msg.get("method"), msg.get("id")
        if method == "elicitation/create":
            # A script cannot answer an approval prompt; ASTRA treats this as deny.
            self._send({"jsonrpc": "2.0", "id": mid, "result": {"action": "decline"}})
        elif method == "ping":
            self._send({"jsonrpc": "2.0", "id": mid, "result": {}})
        else:
            self._send({"jsonrpc": "2.0", "id": mid,
                        "error": {"code": -32601, "message": f"Method not found: {method}"}})

    def request(self, method, params=None):
        """Send one JSON-RPC request and return its result (raises AstraError
        on a JSON-RPC error). Server notifications and requests that arrive
        meanwhile are handled."""
        mid = self._next_id
        self._next_id += 1
        msg = {"jsonrpc": "2.0", "id": mid, "method": method}
        if params is not None:
            msg["params"] = params
        self._send(msg)
        while True:
            m = self._read()
            if "method" in m:
                if "id" in m:
                    self._handle_server_request(m)
                else:
                    self.notifications.append(m)
                    if m["method"] == "notifications/tools/list_changed":
                        self._tools = None
                continue
            if m.get("id") != mid:
                continue
            if "error" in m:
                err = m["error"]
                raise AstraError(f"{method}: {err.get('message')} (code {err.get('code')})")
            return m.get("result", {})

    # ── MCP ──────────────────────────────────────────────────────────────

    def tools(self):
        """The tools ASTRA offers: list of dicts with name, title,
        description, inputSchema and annotations."""
        if self._tools is None:
            self._tools = self.request("tools/list", {}).get("tools", [])
        return self._tools

    def tool_names(self):
        return [t["name"] for t in self.tools()]

    def call(self, tool, **args):
        """Call a tool with keyword arguments.

        Returns the parsed structuredContent when the tool gives one, else
        its text content (str). Raises ToolError when the tool reports an
        error (including a denied approval).
        """
        res = self.request("tools/call", {"name": tool, "arguments": args})
        text = "\n".join(c.get("text", "") for c in res.get("content", [])
                         if c.get("type") == "text")
        if res.get("isError"):
            raise ToolError(f"{tool}: {text or res}")
        if "structuredContent" in res:
            return res["structuredContent"]
        return text

    def resource(self, uri):
        """Text of an MCP resource, e.g. "astra://conventions"."""
        res = self.request("resources/read", {"uri": uri})
        return "\n".join(c.get("text", "") for c in res.get("contents", []))

    def close(self):
        try:
            self._sock.close()
        except OSError:
            pass

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def __repr__(self):
        return f"<astra.Client {self.root} mode={getattr(self, 'mode', None)}>"


def is_running(root=None):
    """True when a running ASTRA serves agents for this data root."""
    if not os.path.exists(token_path(root)):
        return False
    addr = socket_path(root)
    if os.name == "nt":
        return os.path.exists(addr)
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(1.0)
    try:
        s.connect(addr)
        return True
    except OSError:
        return False
    finally:
        s.close()


def connect(root=None, timeout=300.0):
    """Connect to the running ASTRA for `root` (default: the default data
    root); returns a Client. Raises AstraError when ASTRA is not running
    there or refuses the token."""
    return Client(root, timeout)
