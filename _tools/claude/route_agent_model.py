"""quota-router: PreToolUse hook enforcing cheap-by-default model routing for subagents.

Protects Fable 5 usage limits (see memory: preserve-fable-quota-routing).

Agent/Task tool:
  - [hard] or [fable] tag in prompt/description -> force model=fable (conscious escalation)
  - [opus] tag -> force model=opus
  - explicit model sonnet/haiku/opus -> respected
  - missing model, or fable without tag -> rewritten to sonnet

Workflow tool:
  - saved workflows (name) and resumes -> allowed
  - scripts that call agent() with zero `model:` options and no `fable-ok` marker -> denied
    with an instructive reason (the model rewrites the script with routing and retries)
"""
import json
import re
import sys


def emit(obj):
    print(json.dumps(obj))
    sys.exit(0)


def allow():
    sys.exit(0)  # no output -> input passes through unchanged


def main():
    try:
        data = json.load(sys.stdin)
    except Exception:
        allow()

    tool = data.get("tool_name", "")
    ti = data.get("tool_input") or {}
    if not isinstance(ti, dict):
        allow()

    # NOTE: updatedInput is SILENTLY IGNORED for the Agent/Task tool
    # (anthropics/claude-code#39814, closed as not planned) — rewrite is impossible.
    # permissionDecision IS honored, so enforcement is deny-with-instructions:
    # the calling model re-spawns with an explicit model. Tagged spawns pass
    # through (inheriting the Fable session model is then deliberate).
    if tool in ("Agent", "Task"):
        text = "{} {}".format(ti.get("description", ""), ti.get("prompt", ""))
        model = (ti.get("model") or "").lower()

        if model in ("sonnet", "haiku", "opus"):
            allow()  # explicit cheap/mid choice respected

        if re.search(r"\[(hard|fable|opus)\]", text, re.I):
            allow()  # deliberate escalation tag -> inherit/keep fable (or explicit choice)

        # missing model (would inherit Fable) or untagged fable -> reject with instructions
        emit({"hookSpecificOutput": {
            "hookEventName": "PreToolUse",
            "permissionDecision": "deny",
            "permissionDecisionReason": (
                "quota-router: subagent would run on Fable and burn quota. Re-spawn with an "
                "explicit model ('sonnet' default, 'haiku' for mechanical tasks, 'opus' for "
                "complex), or tag the prompt with [hard]/[fable] to deliberately use Fable."
            ),
        }})

    if tool == "Workflow":
        if ti.get("name") or ti.get("resumeFromRunId"):
            allow()  # saved workflows / resumes are pre-vetted
        script = ti.get("script") or ""
        if not script and ti.get("scriptPath"):
            try:
                with open(ti["scriptPath"], encoding="utf-8", errors="replace") as fh:
                    script = fh.read()
            except Exception:
                allow()  # can't inspect -> don't block
        if "agent(" not in script:
            allow()
        if re.search(r"\bmodel\s*:", script) or "fable-ok" in script:
            allow()
        emit({"hookSpecificOutput": {
            "hookEventName": "PreToolUse",
            "permissionDecision": "deny",
            "permissionDecisionReason": (
                "quota-router: this workflow script spawns agents with no model routing — "
                "every agent would inherit Fable and burn quota. Add explicit model opts "
                "(model:'sonnet' or 'haiku' for sweeps/mechanical stages; 'fable'/'opus' only "
                "for verify/judge/hardest reasoning), or use the routed-fanout saved workflow, "
                "or include the marker 'fable-ok' in the script to bypass deliberately."
            ),
        }})

    allow()


if __name__ == "__main__":
    main()
