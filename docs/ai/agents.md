# AI Agents (MCP)

AI agents you already use on this computer, such as Claude Code, Codex,
opencode, Gemini CLI or Claude Desktop, can work with ASTRA through the
Model Context Protocol (MCP). They get the same tools as the built-in
[assistant](assistant.md): they read and change your data and, while ASTRA is
open, use its windows. You see what they do, and risky actions wait for your
approval under the same [permission rules](permissions.md).

## Enabling agent access

Agent access is off until you switch it on:
**Settings / AI / AI agents on this computer / Let AI agents on this computer
use ASTRA (MCP server)**.

Only programs running as your own user can connect. The settings page also
shows the exact command agents have to start (**Command for agents**), which
is the path of your ASTRA executable followed by `--mcp`.

## Connecting agents

**Settings / AI / Connect AI agents...** (which also ticks the checkbox
above) opens a dialog that sets the agents up for you. It lists the agents
it knows, whether each was found on this computer, and exactly what
connecting does:

| Agent | What connecting does |
|---|---|
| Claude Code | runs `claude mcp add --scope user astra -- "<ASTRA>" --mcp` (removing an older `astra` entry first) |
| Codex CLI | adds an `[mcp_servers.astra]` section to `~/.codex/config.toml`, unless one is there already |
| opencode | adds `"astra"` under `"mcp"` in `~/.config/opencode/opencode.json` |
| Gemini CLI | adds `"astra"` under `"mcpServers"` in `~/.gemini/settings.json` |
| Claude Desktop | adds `"astra"` under `"mcpServers"` in `claude_desktop_config.json` |

Agents that were found are ticked. **Connect selected** applies the changes
and reports, per agent, what it did or why it could not.

- Before editing a configuration file, ASTRA copies it to
  `<file>.astra-backup-<date>-<time>` next to the original.
- A JSON file with comments in it is not touched; the dialog asks you to add
  the entry by hand instead.
- If you run ASTRA with its own `ASTRA_DATA_DIR`, that variable is written
  into the agent's entry too, so the agent reaches the same data.
- Restart the agents (or reload their MCP servers) afterwards to pick ASTRA
  up.

The lower half of the dialog always shows the command and a JSON snippet for
setting up any other MCP client by hand.

<div class="media-placeholder screenshot">
  <div class="mp-icon">🖼️</div>
  <div class="mp-caption">Screenshot: the Connect AI agents dialog with Claude Code and Codex found</div>
  <div class="mp-file">assets/images/ai-connect-agents.png</div>
</div>

## Manual setup

Every client starts the same command over stdio: the ASTRA executable with
`--mcp`. In the examples below, replace `/path/to/ASTRA` with the full path
of your executable.

!!! note "AppImages"
    For an AppImage, use the path of the `.AppImage` file itself, e.g.
    `/home/me/Applications/astra-0.10.0-x86_64.AppImage`. Do not use a path
    inside its temporary mount under `/tmp`, which changes on every start.

=== "Claude Code"

    ```
    claude mcp add --scope user astra -- /path/to/ASTRA --mcp
    ```

=== "Codex"

    In `~/.codex/config.toml`:

    ```toml
    [mcp_servers.astra]
    command = "/path/to/ASTRA"
    args = ["--mcp"]
    ```

=== "opencode"

    In `~/.config/opencode/opencode.json`:

    ```json
    {
      "mcp": {
        "astra": {
          "type": "local",
          "command": ["/path/to/ASTRA", "--mcp"],
          "enabled": true
        }
      }
    }
    ```

=== "Other clients"

    Claude Desktop, Gemini CLI, Cursor and most other clients use an
    `mcpServers` map:

    ```json
    {
      "mcpServers": {
        "astra": {
          "command": "/path/to/ASTRA",
          "args": ["--mcp"]
        }
      }
    }
    ```

To use a data directory other than the default, add `ASTRA_DATA_DIR` to the
entry's environment (`env` in the JSON and TOML formats, `environment` for
opencode, `-e ASTRA_DATA_DIR=...` for `claude mcp add`).

## How `ASTRA --mcp` works

The agent starts `ASTRA --mcp` itself and talks to it over standard input and
output. What happens next depends on whether ASTRA is open:

- **ASTRA is open**: the command attaches to the running window and relays
  the agent's requests to it. The agent works in the window you see: it can
  open star detail windows, zoom plots, filter the table and press buttons in
  dialogs, and every approval card appears in that window.
- **ASTRA is not open**: the command opens your data itself and serves the
  data and job tools without a window: reading and editing stars, spectra,
  fits, RV and light-curve data, exporting, fetching from archives and
  running fits. Window, plot and UI tools are not available. Further agents that start meanwhile share this one process
  rather than opening the database a second time.
- **The window opens later**: it takes over, because two programs must not
  edit the same database. The background process steps aside and reattaches
  to the window, a request that was running at that moment is answered with
  a "retry" error, and the status bar says that an agent was using ASTRA in
  the background. The window and UI tools then become available. Some agents
  notice this at once; with others you may need to reconnect (in Claude Code,
  `/mcp`).
- **The window closes while an agent is attached**: the agent carries on
  without a window, with the data and job tools only.

`ASTRA --mcp` refuses to start when AI features or agent access are switched
off in Settings / AI, or by `ASTRA_DISABLE_AI=1`. The agent then receives that
reason as an error message, so it can tell you instead of reporting a broken
server.

## Tools

The tools come in groups. The data and job groups are always there; the
window, plot and generic UI groups only while the ASTRA window is open.

| Group | Tools | Needs the window |
|---|---|---|
| State | `get_state` (what ASTRA is doing: version, data directory, permission mode, open project, selection, windows, waiting dialogs; agents should call it first), `python_info` | no |
| Projects & stars | `list_projects`, `create_project`, `update_project`, `delete_project`, `list_star_fields`, `query_stars`, `find_stars`, `get_star`, `set_star_fields`, `add_stars`, `import_stars_csv`, `copy_stars`, `move_stars`, `delete_stars`, `revert_change` | no |
| Spectra | `list_spectra`, `get_spectrum`, `list_spectral_fits`, `get_spectral_fit`, `set_best_fit`, `flag_spectrum`, `flag_spectral_fit`, `import_spectrum`, `delete_spectrum`, `delete_spectral_fit` | no |
| Radial velocities | `get_rv_curve`, `list_rv_fits`, `get_rv_periodogram`, `add_rv_points`, `set_rv_point_active`, `set_best_rv_fit`, `delete_rv_points`, `delete_rv_fit` | no |
| Photometry & light curves | `get_photometry`, `list_lightcurves`, `get_lightcurve`, `get_periodogram`, `list_lc_fits`, `list_sed_models`, `set_best_lc_fit`, `delete_lc_fit`, `delete_sed_model`, `remove_lightcurve` | no |
| Jobs | `fetch_spectra`, `fetch_lightcurves`, `list_fit_plans`, `start_mass_fit`, `run_spectral_fit`, `run_rv_mcmc`, `list_jobs`, `get_job` (status, result and the tail of the log), `pause_job` (mass fits only), `resume_job`, `cancel_job` | no |
| Export | `export_data` (full-resolution CSV, JSON or `.npy` files plus a manifest, for your own scripts) | no |
| Windows & table | `list_windows`, `open_project`, `close_project`, `open_star_detail`, `open_star_tool`, `open_window`, `close_window`, `table_select`, `table_set_filter`, `table_sort`, `table_set_columns`, `set_theme` | yes |
| Plots | `list_plots`, `plot_set_view`, `plot_reset`, `plot_get_data`, `plot_click`, `plot_export`, `open_plot_tool`, `plot_tool_get_config`, `plot_tool_set_config` | yes |
| Generic UI | `ui_snapshot` (a text outline of a window's controls), `ui_press`, `ui_set_value`, `ui_key`, `ui_list_menus`, `ui_trigger_menu`, `dialog_respond`, `screenshot` | yes |

The generic UI tools reach every part of ASTRA that has no dedicated tool:
`ui_snapshot` lists a window's buttons, fields and lists with a short
reference each, and `ui_press`, `ui_set_value` and `dialog_respond` operate
them as you would. `screenshot` returns an image of a window, a plot or a
single control, which is how an agent looks at a plot.

### Long jobs

Fetches and fits take minutes to hours, so the tools that start them
(`fetch_spectra`, `fetch_lightcurves`, `start_mass_fit`, `run_spectral_fit`,
`run_rv_mcmc`) return a `job_id` at once and the job runs on in the
background. The agent then polls `get_job` for progress, the log and, once
the job is done, its result, and can stop it with `cancel_job` (a mass fit
also with `pause_job` and later `resume_job`, even after ASTRA was
restarted). `list_jobs` shows every job ASTRA knows, including the ones you
started yourself. Fetches and mass fits also appear in ASTRA's own session
lists, so you can follow them there as usual.

Only one local spectral fit or RV MCMC runs at a time, because each uses all
cores; further ones queue behind it. Light-curve sources that need
credentials (ZTF, ATLAS, BlackGEM) only work when those are already set up
in ASTRA (see [Light Curves](../workflows/light-curves.md#one-time-setup)); an agent cannot
enter them, and sources without them are skipped and reported.

`run_python` is reserved for the built-in assistant; agents have their own
ways of running code and use `python_info` to find the [`astra`
package](python.md) instead.

ASTRA also offers three MCP resources: `astra://conventions` (units, error
conventions, time scales, ids), `astra://fields` (every star field with its
unit and whether it is editable) and `astra://python` (how to load the data
from Python).

## Prompting tips

- **Name stars any way you like.** Aliases, Gaia source ids, TIC numbers,
  J-names and coordinates all work; the agent resolves them with
  `find_stars`. Say which project if a star appears in several.
- **Ask it to show you.** "Open the detail window of PG 1618+562 and zoom the
  light curve to the primary eclipse" makes the agent work in the window, so
  you can see what it found.
- **Ask for plots to be looked at.** Agents that accept images can take a
  `screenshot` of a plot and judge it ("is the RV fit of the selected star
  convincing?").
- **For heavy analysis, export.** Tool results thin out long spectra and
  light curves. For full-resolution work, ask the agent to use `export_data`
  or to load the data with the `astra` Python package.
- **Be explicit about scope.** "Set spec_class to sdB for the 12 selected
  stars" is better than "classify these". Edits of more than 20 stars at once
  ask for approval anyway.
- **Let it undo.** "Undo that" works for field edits, added RV points, flags
  and best-fit choices; the agent remembers the change id.
- **Say how much to fetch or fit.** "Fetch LAMOST and SDSS spectra for the
  selected stars, then fit the new ones" is clear; "fetch everything for the
  project" may start a job of several hours. Fetches and mass fits ask for
  approval in ask mode.
- **Read-only for exploring.** Switch to read-only mode when you only want
  answers; the agent can still open windows and zoom plots.
