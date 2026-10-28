# AI Features

ASTRA can work together with AI models. There are three ways in, and you can
use any of them, all of them or none:

| | Built-in assistant | AI agents (MCP) | Python |
|---|---|---|---|
| What it is | a chat panel inside ASTRA | Claude Code, Codex, opencode, Gemini CLI, Claude Desktop and other MCP clients | the `astra` Python package |
| Who runs the model | the endpoint you choose in Settings / AI | the agent's own provider | nobody: plain Python |
| Sees ASTRA's windows | yes | yes, while ASTRA is open | no |
| Changes data | through ASTRA's tools, with your permission | through ASTRA's tools, with your permission | read-only, or through the running ASTRA with `astra.connect()` |
| Page | [AI Assistant](assistant.md) | [AI Agents (MCP)](agents.md) | [Python Access](python.md) |

The assistant and the agents use the same set of tools: read stars, spectra,
fits, RV curves and light curves; edit fields and RV points; fetch from
archives and run fits; open a star's detail window, zoom a plot, filter the table; and press buttons in ASTRA's
own dialogs. Every call goes through the same
[permission rules](permissions.md) and is written to the same activity log.

<div class="media-placeholder screenshot">
  <div class="mp-icon">🖼️</div>
  <div class="mp-caption">Screenshot: the AI assistant next to the star table, with a star's detail window opened by it</div>
  <div class="mp-file">assets/images/ai-overview.png</div>
</div>

## First launch

The first time a version with AI features starts, ASTRA asks once whether AI
agents on this computer may use it. **Allow agents...** switches on the MCP
server and opens the [Connect AI agents](agents.md#connecting-agents) dialog,
**Not now** leaves it off, and **Turn AI off** switches every AI feature off.
You can change all of this later under **Settings / AI**.

## What leaves your computer

- **The built-in assistant** sends what you type, the data it reads with its
  tools and the screenshots it takes to the endpoint you configured, and
  nowhere else. With a local model (Ollama, LM Studio, vLLM on your own
  machine) nothing leaves the computer at all. Each message also carries a
  short context note: the open project, the selected stars and the titles of
  the open windows.
- **AI agents** talk to ASTRA over a local socket that only programs running
  as your user can open. ASTRA itself sends nothing anywhere for them, but the
  agent passes what it reads to its own model provider, under that agent's
  settings and terms.
- **The Python package** reads files on disk and sends nothing.

API keys are stored in `ai/credentials.json` inside ASTRA's
[data directory](../reference/database.md), readable only by your user, or
not stored at all if you give an environment variable instead
(see [setting up a provider](assistant.md#setting-up-a-provider)).

## Turning AI off

There are three levels:

1. **Settings / AI / Enable AI features**. When off, ASTRA shows no AI button
   or chat, accepts no AI agents (and `ASTRA --mcp` refuses to start) and
   sends nothing to any AI service. The assistant and the agents can also be
   switched off separately on the same page.
2. **The environment variable `ASTRA_DISABLE_AI=1`** forces everything off for
   a machine or a whole site, whatever the settings say. The settings page
   then explains why it is greyed out.
3. **A build configured with `-DASTRA_WITH_AI=OFF`** contains none of the AI
   code at all: no settings page, no chat, no MCP server, and no Python
   package installed alongside.

!!! note "Changes are logged and undoable"
    Every tool call by the assistant or an agent is recorded with who made it,
    what it did and how it was permitted. Edits return a change id that the
    agent can undo with `revert_change`. See [Permissions](permissions.md).
