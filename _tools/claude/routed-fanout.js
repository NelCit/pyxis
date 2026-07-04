export const meta = {
  name: 'routed-fanout',
  description: 'Difficulty-routed parallel dispatch: Haiku triage assigns each task a model+effort tier, then fans out — protects Fable/Opus quota',
  whenToUse: 'Any parallel fan-out over a list of tasks where hardcoding one model for all of them would waste quota. Pass args as an array of task strings (or {prompt} objects).',
  phases: [
    { title: 'Triage', detail: 'Haiku scores each task 1-5 for difficulty' },
    { title: 'Dispatch', detail: 'parallel execution at the assigned tier' },
  ],
}

// Tier table — quota-first defaults. Haiku does not support the effort param (omit it).
const TIERS = {
  1: { model: 'haiku' },                    // mechanical: rename, grep, format, lookup
  2: { model: 'sonnet', effort: 'low' },    // routine, well-specified edits
  3: { model: 'sonnet', effort: 'high' },   // standard implementation / analysis
  4: { model: 'opus', effort: 'xhigh' },    // complex, cross-cutting, subtle
  5: { model: 'fable', effort: 'xhigh' },   // architecture, hard debugging, deep reasoning
}

const TRIAGE_SCHEMA = {
  type: 'object',
  required: ['scores'],
  properties: {
    scores: {
      type: 'array',
      items: {
        type: 'object',
        required: ['index', 'score'],
        properties: {
          index: { type: 'integer' },
          score: { type: 'integer', minimum: 1, maximum: 5 },
          reason: { type: 'string', description: 'One short clause' },
        },
      },
    },
  },
}

// Accept args as [taskString|{prompt}] or {tasks: [...]}
const raw = Array.isArray(args) ? args : (args && Array.isArray(args.tasks) ? args.tasks : null)
if (!raw || raw.length === 0)
{
  return { error: 'routed-fanout needs args as a non-empty array of task strings (or {prompt} objects), or {tasks: [...]}' }
}
const tasks = raw.map(t => (typeof t === 'string' ? { prompt: t } : t)).filter(t => t && t.prompt)

phase('Triage')
const listing = tasks.map((t, i) => `[${i}] ${t.prompt.slice(0, 400)}`).join('\n')
let triage = null
try
{
  triage = await agent(
    `Score each task's difficulty for an AI coding agent, 1-5. Bias DOWN when unsure — cheaper tiers are preferred and hard tasks are the exception.\n` +
    `1 = mechanical (rename, grep, format, single lookup)\n` +
    `2 = routine, fully specified small change\n` +
    `3 = standard implementation or analysis needing judgment\n` +
    `4 = complex: cross-cutting, subtle correctness, adversarial verification\n` +
    `5 = architecture, deep debugging, novel design reasoning\n\n` +
    `Tasks:\n${listing}\n\nReturn a score for every index 0..${tasks.length - 1}.`,
    { label: 'triage', phase: 'Triage', model: 'haiku', schema: TRIAGE_SCHEMA }
  )
}
catch (e) { triage = null }

const scoreByIndex = new Map()
if (triage && Array.isArray(triage.scores))
{
  for (const s of triage.scores) scoreByIndex.set(s.index, s)
}
// Fallback for missing scores: tier 3 (sonnet/high) — never accidentally Fable
const routed = tasks.map((t, i) => {
  const s = scoreByIndex.get(i)
  const score = s && s.score >= 1 && s.score <= 5 ? s.score : 3
  return { ...t, index: i, score, reason: (s && s.reason) || 'no triage score — defaulted', tier: TIERS[score] }
})

const counts = {}
for (const r of routed) counts[`${r.tier.model}${r.tier.effort ? '/' + r.tier.effort : ''}`] = (counts[`${r.tier.model}${r.tier.effort ? '/' + r.tier.effort : ''}`] || 0) + 1
log(`Routing: ${Object.entries(counts).map(([k, v]) => `${v}× ${k}`).join(', ')}`)

phase('Dispatch')
const results = await parallel(routed.map(r => () =>
  agent(r.prompt, {
    label: `t${r.score}:${r.prompt.slice(0, 40)}`,
    phase: 'Dispatch',
    model: r.tier.model,
    ...(r.tier.effort ? { effort: r.tier.effort } : {}),
  }).then(out => ({ index: r.index, score: r.score, model: r.tier.model, effort: r.tier.effort || null, reason: r.reason, result: out }))
))

const done = results.filter(Boolean)
log(`${done.length}/${routed.length} tasks completed`)
return {
  routing: routed.map(r => ({ index: r.index, score: r.score, model: r.tier.model, effort: r.tier.effort || null, reason: r.reason })),
  results: done,
}
