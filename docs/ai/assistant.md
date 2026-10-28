# AI Assistant

The assistant is a chat panel inside ASTRA. It can read your data, answer
questions about it and work in ASTRA with you: open a star's detail window,
zoom a plot, filter the table, mark a best fit, edit fields, or run a short
Python analysis. You watch everything it does, and risky actions wait for
your OK.

## Opening it

- the round button at the bottom right of the star table, or
- **View / AI Assistant**, or <kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>A</kbd>.

The chat opens as a panel on the right of the main window. Like any panel it
can be moved to the left side or undocked. The same shortcut hides it again.

If no provider is set up yet, the panel shows a short setup card with an
**Open AI settings** button instead of the chat.

<div class="media-placeholder screenshot">
  <div class="mp-icon">🖼️</div>
  <div class="mp-caption">Screenshot: the AI Assistant panel with a conversation, tool calls and a returned plot</div>
  <div class="mp-file">assets/images/ai-chat.png</div>
</div>

## Setting up a provider

**Settings / AI**, group **AI assistant in ASTRA**:

| Field | What to enter |
|---|---|
| **Provider** | Anthropic, OpenAI, Google Gemini or Custom endpoint. Choosing one fills in the fields below with its defaults. |
| **Endpoint URL** | the API base URL, e.g. `https://api.anthropic.com` |
| **API format** | **Anthropic Messages API** or **OpenAI Chat Completions API** |
| **Authentication** | **x-api-key header** or **Authorization: Bearer** |
| **API key** | the key, or `env:VARIABLE_NAME` |
| **Model** | the model id; type it or pick one after **Fetch models** |

**Fetch models** asks the endpoint which models it offers and fills the model
list. **Test** sends a one-word request and shows the answer, or the error
the endpoint returned. **Longest answer (tokens)** caps the length of a single
reply.

### Custom endpoints

**Custom endpoint** covers anything that speaks one of the two API formats:

- **University or company proxies** such as LiteLLM. Most speak the OpenAI
  format with a Bearer key; some also offer the Anthropic format. Use the
  model ids exactly as the proxy lists them (Fetch models shows them).
- **Local models**: Ollama, LM Studio and vLLM all speak the OpenAI format.
  Point the endpoint at the local server, for example
  `http://localhost:11434/v1` for Ollama. A local server usually needs no key;
  leave the field empty or enter anything.

Google Gemini is reached through its OpenAI-compatible endpoint, which the
preset fills in.

!!! tip "Pick a model that handles tools well"
    The assistant works by calling ASTRA's tools, often many in a row. Small
    local models can chat but tend to get lost in longer tool sequences.

### Keys from the environment

Instead of the key itself you can enter `env:ANTHROPIC_API_KEY` (or any other
variable name). ASTRA then reads the key from that environment variable each
time it is needed and never stores it. Stored keys go to
`ai/credentials.json` in the [data directory](../reference/database.md),
readable only by your user, not to ASTRA's settings file.

<div class="media-placeholder screenshot">
  <div class="mp-icon">🖼️</div>
  <div class="mp-caption">Screenshot: Settings / AI with a custom OpenAI-format endpoint filled in</div>
  <div class="mp-file">assets/images/ai-settings.png</div>
</div>

## What the assistant sees

With each message ASTRA adds a short context note that you do not see in the
chat:

- the open project (name, id, number of stars), or that the project overview
  is shown,
- the selected stars (the first ten by name and id, plus the total),
- the titles of the other open windows.

So "fit the selected star" or "what is wrong with this plot?" work without
spelling out which star or window you mean. Anything beyond that, the
assistant has to look up with its tools: star fields, spectra, fits, RV
points, light curves, periodograms, or a screenshot of a window. What it
reads and every screenshot it takes are sent to your endpoint.

## What it can do

The assistant has the same tools as an [external agent](agents.md#tools), plus
`run_python`. In short:

- **Read** projects, stars, spectra and spectral fits, RV curves and orbit
  fits, light curves and their fits, SED fits, photometry and periodograms.
- **Show** things: open projects and star detail windows, open a star's tools
  (spectral fitting, RV inspector, SED, light curves, CMD, ...), zoom and
  reset plots, select, filter and sort the star table, set its columns, open
  the plotting tool with a configuration, switch the theme.
- **Edit**: set star fields, add and import stars, copy and move stars, add
  RV points, include or exclude points, mark best fits, flag spectra and fits,
  import a spectrum file, export data to files.
- **Fetch and fit**: download spectra from the archives and survey light
  curves, run single spectral fits, mass fits from your saved fitting plans
  and RV MCMC orbit searches, and follow, pause or cancel them. These run as
  background jobs (see [long jobs](agents.md#long-jobs)).
- **Delete** stars, projects, spectra, fits, RV points and light curves, with
  your approval.
- **Use any window** through generic controls: read a dialog's fields, fill
  them in, press its buttons, choose menu entries. This is how it reaches
  features that have no dedicated tool.

Each tool call appears in the chat as a grey line with a ✓ or ✗ when done;
click **Show** to see the arguments and the result. Screenshots and plots the
assistant produces appear inline.

The menu at the top of the panel switches the
[permission mode](permissions.md) (ask, auto, read-only) without opening the
settings. Below it you see the provider and model in use; at the bottom, the
tokens used in this conversation and, where the provider reports it, the cost.

**Stop** interrupts the assistant: a running tool is told to stop and nothing
further is requested from the model.

## Running Python

`run_python` lets the assistant write and run a Python script for anything
ASTRA's tools do not cover: a custom statistic, a fit of its own, a
publication plot. The script runs with the [`astra` package](python.md)
importable, so it can load your data directly, and with numpy, scipy,
matplotlib, astropy and pandas available in ASTRA's managed environment.
PNG files the script saves are shown in the chat.

Set this up once under **Settings / AI**, group **Python for the assistant**:

- **Set up ASTRA's Python environment** creates a virtual environment under
  `ai/python-venv` in the data directory and installs numpy, scipy,
  matplotlib, astropy, pandas and zstandard into it. It needs a Python 3 on
  the system to start from; the log of the installation is shown while it
  runs. Press it again later to update the packages.
- **Interpreter**: leave empty for that managed environment, or give the path
  of your own Python (a conda environment, say). Without either, ASTRA falls
  back to a `python3` on your `PATH`.
- **Time limit per run**: the default limit for one script (300 s unless you
  change it). A script running longer is stopped.

Each conversation gets its own scratch folder under `ai/python-work`, which is
the script's working directory.

!!! warning "Python code is not sandboxed"
    A script the assistant runs can do anything your user account can. That
    is why `run_python` is an *execute* action: in ask mode every run waits for
    your approval, and **Show details** on the approval card shows the full
    script. Data changes should go through ASTRA's
    tools, not through Python, so they are logged and can be undone.

## History and new chats

Conversations are saved as you go, under `ai/chats` in the data directory.

- **History** lists the saved conversations by date and title; choose one to
  continue it. Images are not kept in saved chats; a restored conversation
  shows where they were.
- **New chat** starts an empty conversation. The previous one stays in the
  history.

Starting a new chat also ends any "Allow for this session" permissions you
gave in the old one.
