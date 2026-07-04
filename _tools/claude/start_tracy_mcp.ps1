# Tracy MCP server launcher (Windows). See initialize_claude.md §5 for the build steps.
# Serves zone/GPU-zone/frame statistics from .tracy captures over Streamable HTTP
# (port 47380 by default; TRACY_MCP_PORT to override). Claude Code registers the
# endpoint http://localhost:47380/mcp — run this script whenever you are profiling.
#
# Layout expected (created by the setup steps):
#   $env:TRACY_MCP_HOME\build\python\Release\TracyServerBindings*.pyd   (CMake build output)
#   $env:TRACY_MCP_HOME\extra\mcp\tracy_mcp.py + .venv with the `mcp` package

if (-not $env:TRACY_MCP_HOME) { $env:TRACY_MCP_HOME = "$env:USERPROFILE\.claude\mcp-tools\tracy" }
$bindings = Join-Path $env:TRACY_MCP_HOME "build\python\Release"
$mcpDir   = Join-Path $env:TRACY_MCP_HOME "extra\mcp"

if (-not (Get-ChildItem $bindings -Filter "TracyServerBindings*.pyd" -ErrorAction SilentlyContinue)) {
    Write-Error "TracyServerBindings*.pyd not found in $bindings — build it first (initialize_claude.md §5, Tracy)."
    exit 1
}
$env:PYTHONPATH = if ($env:PYTHONPATH) { "$env:PYTHONPATH;$bindings" } else { $bindings }

# Where .tracy capture files are read from (defaults to the repo's captures/ dir).
if (-not $env:TRACY_CAPTURES_DIR) {
    $env:TRACY_CAPTURES_DIR = Join-Path (Split-Path (Split-Path $PSScriptRoot)) "captures"
}

& (Join-Path $mcpDir ".venv\Scripts\python.exe") (Join-Path $mcpDir "tracy_mcp.py") @args
