#pragma once

/*  `astra --mcp`: the command AI agents (Claude Code, Codex, opencode, ...)
 *  are configured to start. It speaks MCP over stdin/stdout.
 *
 *  If an ASTRA window (or another `astra --mcp`) already serves this data
 *  root, it relays bytes to it, so the agent drives the open window. If not,
 *  it opens the database itself and serves the data and job tools without a
 *  window, and listens on the local socket so later agents share it.
 *
 *  Refuses to start when AI features or MCP access are switched off in
 *  ASTRA's settings. Everything ASTRA would print to stdout goes to stderr:
 *  stdout belongs to the protocol.
 *
 *  Returns -1 when argv is not `--mcp`.                                     */

int runMcpMode(int argc, char** argv);
