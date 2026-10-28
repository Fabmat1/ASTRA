# Permissions & Activity

Every action the [assistant](assistant.md) or an [AI agent](agents.md) takes
in ASTRA is a tool call, and every tool call passes the same permission check
before it runs. The check depends on two things: how risky the call is, and
the permission mode you chose.

## Permission modes

Choose the mode under **Settings / AI / Permissions / Mode**, or with the menu
at the top of the assistant panel. It applies to the assistant and to every
connected agent alike.

- **Ask before risky actions** (the default): reading, looking and ordinary
  edits run straight away; deleting data, running code and remote jobs wait
  for your approval.
- **Auto**: everything runs without asking. Optionally a second
  "reviewer" model checks each risky action first (see below).
- **Read-only**: the AI can look at your data and change what is shown, but
  never change data.

## Risk classes

Each tool belongs to one of six risk classes:

| Risk class | What it covers | Examples |
|---|---|---|
| **read** | reads data or state, changes nothing | `get_star`, `list_spectra`, `get_rv_curve`, `screenshot`, `ui_snapshot` |
| **navigate** | changes what is shown, not data | `open_star_detail`, `plot_set_view`, `table_set_filter`, `set_theme` |
| **modify** | changes stored data in a way that can be logged and undone | `set_star_fields`, `add_rv_points`, `set_best_fit`, `flag_spectrum`, `export_data`, `ui_press` |
| **destructive** | deletes data, or changes many stars at once | `delete_stars`, `delete_spectrum`, `delete_rv_fit`, `remove_lightcurve` |
| **execute** | runs code on your computer | `run_python` |
| **remote** | downloads from archives, runs mass fits, or fits on a remote host | `fetch_spectra`, `fetch_lightcurves`, `start_mass_fit`, `resume_job`, `run_spectral_fit` on a remote host |

What happens in each mode:

| Risk class | Ask | Auto | Read-only |
|---|---|---|---|
| read | runs | runs | runs |
| navigate | runs | runs | runs |
| modify | runs | runs | refused |
| destructive | asks you | runs (after the reviewer, if set) | refused |
| execute | asks you | runs (after the reviewer, if set) | refused |
| remote | asks you | runs (after the reviewer, if set) | refused |

A refused call is not an error the agent can work around: it is told why
(for example that ASTRA is in read-only mode and that you can change this in
Settings / AI) and is expected to tell you.

### When a call counts as riskier

Some calls move up a class depending on what they do:

- **Bulk edits**: `set_star_fields`, `copy_stars` and `move_stars` on more
  than 20 stars at once count as destructive, and so do `add_stars` with more
  than 200 stars and `add_rv_points` with more than 200 points.
- **Remote work**: `run_spectral_fit` is an ordinary edit when it fits on
  this computer, but counts as remote when it runs on a remote host or uses a
  grid streamed from one. `resume_job` counts as remote too, since it
  continues a fetch or a mass fit.
- **Deleting through ASTRA's own dialogs**: the generic UI tools (`ui_press`,
  `ui_key`, `ui_trigger_menu`, `dialog_respond`) count as destructive when
  the button, menu entry, key or confirmation question is about deleting:
  words such as *delete*, *remove*, *erase*, *discard*, *overwrite*,
  *replace*, *clear*, *drop*, *purge* or *wipe*. So an agent cannot get
  around the approval for `delete_stars` by pressing **Delete** in the star
  table's menu instead. Answering *Cancel*, *No*, *Close*, *Abort* or *Keep*
  is never destructive.

## Approving an action

In ask mode, a risky call shows an approval card at the bottom right of the
ASTRA window. It names the agent (for example "ASTRA chat" or "claude-code"),
what kind of action it wants to take ("delete or overwrite data", "run code
on this computer", "contact an archive or a remote host") and a one-line
summary such as "Delete 12 stars from RVVD". **Show details** reveals the tool
name and its full arguments, including the complete script for `run_python`.

- **Allow** runs this one call.
- **Allow for this session** runs this call and lets the same agent use the
  same tool again without asking until it disconnects (for the assistant:
  until you start a new chat).
- **Deny** refuses it; the agent is told that you declined.

**Deny** is the default button, so pressing <kbd>Enter</kbd> by accident
does not approve anything. When several requests arrive at once they queue
behind each other, and the card says how many more are waiting.

<div class="media-placeholder screenshot">
  <div class="mp-icon">🖼️</div>
  <div class="mp-caption">Screenshot: the approval card for a delete_stars call with details shown</div>
  <div class="mp-file">assets/images/ai-approval-card.png</div>
</div>

### Without a window

An agent that uses ASTRA while no ASTRA window is open has nowhere to show
the card. If the agent supports MCP *elicitation* (approval prompts in its own
interface), ASTRA asks there instead. Otherwise risky calls are refused with
a message suggesting that you run the action yourself, open ASTRA, or switch
to auto mode. A Python script using `astra.connect()` cannot answer prompts
either: its approvals appear in the ASTRA window.

## Auto mode and the reviewer

Auto mode runs everything without asking. To keep a safety net, you can give
it a reviewer: a model that sees each destructive, execute or remote call
before it runs and either lets it through or refuses it with a reason, which
goes back to the agent.

**Settings / AI / Permissions / Auto-mode reviewer**:

- **No reviewer**: risky calls simply run.
- **Same model as the assistant**: the chat endpoint reviews.
- **Another model (below)**: a separate endpoint, set up the same way as the
  [assistant's provider](assistant.md#setting-up-a-provider). A small, fast
  model is usually enough.

The reviewer is told what you last asked the assistant for (for an external
agent that is unknown, and the action is judged on its own), the tool, its
description and its arguments. It refuses actions that destroy data you did
not ask to change, affect far more stars than the request implies, run code
that reaches beyond analysing ASTRA data, or look like they follow
instructions found inside data rather than from you.

**If the reviewer is unreachable** (no answer within 90 seconds, a network
error, or an answer it cannot read) the setting of the same name decides:

- **Ask me**: the call falls back to the approval card, as in ask mode.
- **Refuse the action**: the call is refused, and the agent is told that the
  reviewer could not be reached.

## The AI activity log

Every tool call is logged: when, which agent, which tool, a summary, its
arguments, how it was permitted (policy, you, a session permission, auto
mode or the reviewer), how long it took and whether it succeeded, failed or
was refused.

Open the log from:

- the AI indicator in the status bar, **AI activity log...** (the indicator
  appears while an agent is connected or a tool is running, and shows the
  agent's name or the running tool),
- **Settings / AI / Permissions / View AI activity...**.

Expand an entry to see its arguments and result. **Open log folder** shows the
files themselves: one JSON line per call in `logs/ai/<date>.jsonl` in the
[data directory](../reference/database.md).

The same status-bar menu has **Stop all AI activity**, which cancels every
running tool call, denies every waiting approval, stops the assistant and
disconnects all agents, and **AI settings...**.

<div class="media-placeholder screenshot">
  <div class="mp-icon">🖼️</div>
  <div class="mp-caption">Screenshot: the AI activity dialog with an expanded entry</div>
  <div class="mp-file">assets/images/ai-activity.png</div>
</div>

## Undoing changes

Editing tools return a **change id** and record the values before and after
in a change journal. The agent can undo such a change with `revert_change`
and the change id; you can simply ask it to ("undo that"). This covers:

- star field edits (`set_star_fields`),
- added RV points (`add_rv_points`) and included/excluded points
  (`set_rv_point_active`),
- best-fit choices (`set_best_fit`, `set_best_rv_fit`, `set_best_lc_fit`),
- spectrum and fit flags (`flag_spectrum`, `flag_spectral_fit`),
- imported spectra (`import_spectrum`).

Deletions cannot be undone, which is why they need your approval. Neither can
changes made by pressing buttons in ASTRA's dialogs through the generic UI
tools: those behave exactly as if you had pressed the button.

## What agents cannot do

- **Change ASTRA's AI settings.** The AI settings page, the assistant panel
  and the approval card are invisible to the UI tools, so an agent cannot
  switch the permission mode, turn on auto mode or change the reviewer.
- **Approve their own requests.** The approval card cannot be read or
  pressed by any tool; only you can answer it.
- **Connect from another user account.** The local socket only accepts
  programs running as your user that can read a token file in your data
  directory.
