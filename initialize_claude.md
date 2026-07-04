# Claude Code Environment — Portable Setup (Pyxis)

> Complete, machine-agnostic specification of the Claude Code environment used to develop Pyxis.
> Windows 10/11. Everything **project-level ships with this repo** (`.claude/`, `.serena/`, `.clangd`, `CLAUDE.md`, `_tools/claude/`) and needs no action.
> Everything **user-level** is reproduced by the steps below on any machine. Run them top to bottom on a fresh box; each section is independent.
>
> Conventions: PowerShell commands; `claude` = the Claude Code CLI (bundled with the VS Code extension under `resources\native-binary\claude.exe` if not on PATH). After any hook/MCP/skill change, fully restart the Claude Code host (quit, not reload).

---

## 1. Prerequisites

```powershell
winget install Git.Git Python.Python.3.12 OpenJS.NodeJS.LTS astral-sh.uv BurntSushi.ripgrep.MSVC
winget install LLVM.LLVM                 # clangd + clang-cl (project toolchain)
winget install Microsoft.WinDbg          # cdbX64.exe for the windbg MCP server
winget install BaldurKarlsson.RenderDoc  # GUI frame debugging (see §5 limitation note)
winget install Docker.DockerDesktop      # optional — only for the NVIDIA Kit MCP module (§5)
# Visual Studio (Community/Build Tools) with C++ workload — required by Pyxis itself and the Tracy build
```

Notes: winget's `uv`/`uvx` PATH aliases are unreliable — the exes live under
`%LOCALAPPDATA%\Microsoft\WinGet\Packages\astral-sh.uv_*\`; use absolute paths in MCP registrations.

## 2. User settings — `%USERPROFILE%\.claude\settings.json`

Recommended baseline (model/effort are personal choices; the hooks are the load-bearing part):

```json
{
  "model": "claude-fable-5[1m]",
  "effortLevel": "xhigh",
  "hooks": {
    "PreToolUse": [
      { "matcher": "Bash",
        "hooks": [{ "type": "command", "command": "rtk hook claude" }] },
      { "matcher": "Agent|Task",
        "hooks": [{ "type": "command", "command": "python \"%USERPROFILE%\\.claude\\hooks\\route_agent_model.py\"" }] },
      { "matcher": "Workflow",
        "hooks": [{ "type": "command", "command": "python \"%USERPROFILE%\\.claude\\hooks\\route_agent_model.py\"" }] }
    ]
  }
}
```

Use an absolute `python.exe` path in the hook commands if `python` is not guaranteed on PATH.
(RTK's `rtk init -g` writes its own Bash hook entry — see §3.)

## 3. RTK — token compression (60–90% on CLI output)

```powershell
# download rtk-x86_64-pc-windows-msvc.zip from github.com/rtk-ai/rtk releases
# → extract rtk.exe to %USERPROFILE%\.local\bin (add that dir to user PATH)
rtk init -g --auto-patch     # installs the Bash PreToolUse hook + ~\.claude\RTK.md; restart afterwards
rtk gain                     # savings analytics; rtk discover finds missed opportunities
```

Scope: only the Bash tool is compressed; PowerShell-tool calls and built-in Read/Grep/Glob bypass it.

## 4. Subagent quota routing (protects the expensive main-loop model)

Purpose: parallel subagents must never silently inherit the expensive session model (Fable/Opus);
fan-outs default to cheap tiers, escalation is explicit. Two portable assets ship in this repo:

```powershell
New-Item -ItemType Directory -Force "$env:USERPROFILE\.claude\hooks", "$env:USERPROFILE\.claude\workflows" | Out-Null
Copy-Item _tools\claude\route_agent_model.py "$env:USERPROFILE\.claude\hooks\"
Copy-Item _tools\claude\routed-fanout.js     "$env:USERPROFILE\.claude\workflows\"
```

Enforced behavior (hook, registered in §2):

| Spawn | Result |
|---|---|
| Agent/Task with explicit `sonnet`/`haiku`/`opus` model | allowed |
| Prompt tagged `[hard]`/`[fable]`/`[opus]` | allowed — deliberate escalation |
| No model and untagged (would inherit session model), or untagged `fable` | **denied** with re-spawn instructions |
| Workflow scripts calling `agent()` with zero `model:` opts and no `fable-ok` marker | **denied** with instructions |
| Saved workflows / resumes | allowed |

Design constraint: PreToolUse `updatedInput` is silently ignored for the Agent tool
(anthropics/claude-code#39814, closed as not planned) — hence deny-with-instructions, which IS honored,
instead of silent rewrite. `routed-fanout` (invoke: `Workflow({name:"routed-fanout", args:[...tasks]})`)
adds difficulty-based dispatch: a Haiku triage scores each task 1–5 → `haiku / sonnet+low / sonnet+high /
opus+xhigh / fable+xhigh`; triage failure falls back to Sonnet, never the expensive tier.

## 5. MCP servers

All at user scope. `$UV` below = absolute path to `uv.exe`, `$UVX` = `uvx.exe` (see §1 note);
`$TOOLS = "$env:USERPROFILE\.claude\mcp-tools"`.

### Remote (no local install)

```powershell
claude mcp add -s user -t http microsoft-learn https://learn.microsoft.com/api/mcp
claude mcp add -s user -t http deepwiki https://mcp.deepwiki.com/mcp
claude mcp add -s user -t http context7 https://mcp.context7.com/mcp   # keyless tier; API-key header optional
# GitHub: OAuth flow is unreliable — use gh-CLI token header auth instead:
gh auth login   # once, browser flow; needs repo+workflow scopes
claude mcp add -s user -t http github https://api.githubcopilot.com/mcp/ --header "Authorization: Bearer $(gh auth token)"
# re-run the add if the gh token ever rotates
```

### Local, package-managed

```powershell
& $UV tool install -p 3.12 serena-agent          # C++ semantic navigation (drives clangd)
claude mcp add -s user serena -- "$env:USERPROFILE\.local\bin\serena.exe" start-mcp-server --context claude-code

& $UV tool install openusd-mcp --with usd-core   # read/inspect .usda/.usdc stages
claude mcp add -s user openusd -- "$env:USERPROFILE\.local\bin\openusd-mcp.exe"

claude mcp add -s user windbg -- $UVX mcp-windbg                          # crash dumps via cdb
claude mcp add -s user image-compare -- cmd /c npx -y mcp-image-compare-server  # pixel diffs (regression triage)
claude mcp add -s user vfx-parsers -- cmd /c npx -y mcp-vfx-parsers            # .mtlx / .usda / .nk parsing
```

### Local, from source (clones under `$TOOLS`)

```powershell
# usd-write — ~79 USD authoring tools (griptape-ai/usd-mcp-server)
git clone --depth 1 https://github.com/griptape-ai/usd-mcp-server "$TOOLS\usd-mcp-server"
& $UV venv "$TOOLS\usd-mcp-server\.venv" --python 3.12
& $UV pip install -e "$TOOLS\usd-mcp-server" --python "$TOOLS\usd-mcp-server\.venv\Scripts\python.exe"
& $UV pip install "usd-core==25.11"          --python "$TOOLS\usd-mcp-server\.venv\Scripts\python.exe"
claude mcp add -s user usd-write -- "$TOOLS\usd-mcp-server\.venv\Scripts\usd-mcp.exe" mcp-serve

# openexr — EXR/TX/DPX metadata, channels, pixel stats (chordee/mcp-server-openexr)
git clone --depth 1 https://github.com/chordee/mcp-server-openexr "$TOOLS\mcp-server-openexr"
Push-Location "$TOOLS\mcp-server-openexr"; & $UV sync; Pop-Location
claude mcp add -s user openexr -- $UV run --directory "$TOOLS\mcp-server-openexr" main.py

# tracy — zone/GPU-zone/frame stats from .tracy captures (upstream wolfpld/tracy extra/mcp)
git clone --depth 1 https://github.com/wolfpld/tracy "$TOOLS\tracy"
cmake -B "$TOOLS\tracy\build" -S "$TOOLS\tracy" -G "Visual Studio 18 2026" -A x64 `
      -DTRACY_CLIENT_PYTHON=ON -DTRACY_STATIC=OFF -DNO_LTO=ON     # adjust -G to your VS; NO_LTO required (VS rejects Tracy's IPO)
cmake --build "$TOOLS\tracy\build" --config Release --target TracyServerBindings
& $UV venv "$TOOLS\tracy\extra\mcp\.venv" --python 3.12
& $UV pip install mcp --python "$TOOLS\tracy\extra\mcp\.venv\Scripts\python.exe"
claude mcp add -s user -t http tracy http://localhost:47380/mcp
# start when profiling: _tools\claude\start_tracy_mcp.ps1 (reads .tracy files from <repo>\captures\)
```

### Optional module — NVIDIA Kit MCP (only for Omniverse Kit-extension work)

```powershell
git clone --depth 1 https://github.com/NVIDIA-Omniverse/kit-usd-agents "$TOOLS\kit-usd-agents"
# requires: Docker Desktop running + an NVIDIA API key from build.nvidia.com
$env:NVIDIA_API_KEY = "<your key>"
& "$TOOLS\kit-usd-agents\source\mcp\kit_mcp\build-docker.bat"; & "$TOOLS\kit-usd-agents\source\mcp\kit_mcp\run.bat"
claude mcp add -s user -t http kit-dev-mcp http://localhost:9902/mcp
# sibling servers in the same repo: omni_ui_mcp (port docs in its README), usd_code_mcp, isaacsim_mcp
```

### Known limitations (stable facts, verified 2026-07-04)

- **RenderDoc agent tooling is not possible against official builds**: no official Windows
  distribution (installer, winget, or portable zip) ships `renderdoc.pyd`, and the embedded
  interpreter is Python 3.6 while MCP SDKs need ≥3.10 in-process. Community tools
  (`renderdoc-mcp`, `renderdoc-skill`/rdc-cli) all require that module. Only path: build RenderDoc
  from source (Qt toolchain) with python bindings against a modern Python. RenderDoc GUI use is unaffected.
- **RTX Remix Toolkit** has a built-in MCP (`http://127.0.0.1:8000/sse`, auto-starts with the Toolkit) —
  register only if you use Remix, otherwise it is a permanently-offline endpoint.
- **NVIDIA USD Search API** is an enterprise k8s/hosted service (client: `pip install usd-search-client`),
  not workstation software.

## 6. Skills

- **Project skills ship with this repo** (`.claude/skills/`): the 8 Pyxis audit skills
  (api-surface-check, ci-golden-file-check, coding-rules-enforce, flecs-conventions-audit,
  ingest-parity-check, milestone-exit-criteria, profiler-scope-lint, rfc-required) and
  `slang-debugging-reference` (distilled from shader-slang/slang's upstream CLAUDE.md). Nothing to do.
- **NVIDIA-verified skills** (signed catalog, per machine/agent — not committed here for license hygiene):

```powershell
npx -y skills add nvidia/skills --skill omniverse-usd-performance-tuning --agent claude-code --yes
npx -y skills add nvidia/skills --skill omniverse-realtime-viewer --agent claude-code --yes
```

- **ovrtx skills** (NVIDIA proprietary, 15 skills covering the ovrtx API): user-level in
  `%USERPROFILE%\.claude\skills\ovrtx\` — copy from a licensed source. Format fix required and already
  known: each file must be named `SKILL.md` (not `SKILLS.md`) **with the YAML frontmatter as the very
  first lines** (hoist it above NVIDIA's HTML license comment, keeping the comment in the body).
- **Skill security scanning** — scan any third-party skill before adopting:

```powershell
& $UV tool install git+https://github.com/NVIDIA/skillspector.git
skillspector scan <skill-dir> --no-llm
# Known false positive: "P6 prompt extraction" fires on render-output vocabulary (e.g. ovrtx
# reading-render-output's GPU buffer-lifetime note). Read the flagged line before believing it.
```

- **Plugins**:

```powershell
claude plugin marketplace add anthropics/claude-plugins-official
claude plugin install clangd-lsp@claude-plugins-official --scope user
# repo already provides .clangd → build/dev/compile_commands.json; zero further config
```

## 7. Serena project activation

Serena's project memories ship with this repo (`.serena/memories/`: core, tech_stack,
suggested_commands, conventions, task_completion). On a new machine just activate once:
ask Claude to run Serena's `activate_project` on the repo path (or run any Serena tool — it
prompts). Onboarding is already done; do **not** re-run it. Sanity check: `serena memories check`.

## 8. Verification checklist (after a full restart)

```powershell
claude mcp list        # expect: microsoft-learn, deepwiki, context7, github, serena, openusd,
                       # usd-write, openexr, windbg, image-compare, vfx-parsers connected;
                       # tracy (and kit-dev-mcp) show failed unless their local server is running — by design
rtk gain               # RTK hook active after restart
ccusage                # optional: npm i -g ccusage — quota-burn analytics
```

Hook probe (should print a deny decision):
`echo '{"tool_name":"Agent","tool_input":{"prompt":"x"}}' | python "%USERPROFILE%\.claude\hooks\route_agent_model.py"`

## 9. Appendix — evaluated and rejected (research record, 51 verification agents, 2026-07-04)

| Tool | Reason |
|---|---|
| claude-code-router (musistudio) | Mature scenario proxy, but no difficulty routing, no `effort` control, man-in-the-middle for all traffic; superseded by §4 |
| claude-model-router-hook (tzachbon) | Keyword heuristics, bash-only (no Windows), advisory-only for subagents; idea absorbed into §4 |
| RouteLLM / NotDiamond / OpenRouter auto / Bedrock prompt routing / Morph | Real learned routers at the API-gateway layer — cannot see Claude Code subagents, none route `effort` |
| nsys_profiler_mcp | Repository 404 — dead or never existed |
| shader-techniques & mcpmarket GPU skills | Marketing shells, no substantive content, custom licenses |
| Learned fact | No graphics-technique skills exist in any public skill marketplace (verified gap — Pyxis-specific technique skills must be authored in-house) |
