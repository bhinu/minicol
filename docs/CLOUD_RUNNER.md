# Cloud worker for minicol

The prepared GitHub Actions workflow is `.github/workflows/cloud-agent.yml`; `docs/cloud-agent-workflow.yml` is a reviewable copy. GitHub-hosted Ubuntu runners can work while the owner's laptop is off. The worker is hourly, at minute 17 UTC, and also supports manual dispatch. GitHub schedules can be delayed; this is periodic development, not an always-running server.

## Activate

1. Grant the local GitHub CLI workflow-upload permission: `gh auth refresh -h github.com -s workflow`. Complete GitHub's browser authorization. Push the prepared workflow commit to main.
2. In GitHub repository Settings → Secrets and variables → Actions, add a **repository secret** named `OPENAI_API_KEY`. Use an OpenAI API project key with funded billing and suitable model access. Never paste the key into chat, the repository, a workflow file, or logs. API usage is billed separately from a ChatGPT subscription. Configure a project budget/alerts in the API dashboard.
3. Add a **repository variable** `MINICOL_AGENT_ENABLED` with value `true`. Without it, the generation job is skipped. Optional variable `MINICOL_CODEX_MODEL` selects a model available to the API project; leaving it empty uses the CLI default.
4. Open Actions → Minicol cloud agent → Run workflow on main. Verify all three jobs (generate, validate, publish) succeed and the resulting commit author is Bhinu Santosh / GitHub account bhinu.
5. Once that first run succeeds, disable the older local "Improve minicol storage engine" hourly automation to avoid competing workers. Stop any manual CLI loop as well.

An existing repository admin can perform steps 2–4 through GitHub's web UI, or use `gh secret set OPENAI_API_KEY --repo bhinu/minicol` (interactive, secret not on the command line), `gh variable set MINICOL_AGENT_ENABLED --body true --repo bhinu/minicol`, and `gh workflow run cloud-agent.yml --repo bhinu/minicol --ref main`.

## How it works

- **Generate** checks out main with a read-only token and no persisted credentials. Official OpenAI Codex action uses a pinned action commit, CLI 0.165.0, a workspace permission profile, and `drop-sudo`. Only that action receives the API credential through its protected proxy. A trusted exporter copied before agent execution captures a patch (at most 1 MB).
- **Validate** uses a fresh Ubuntu runner with no API key and a read-only GitHub token. It applies the candidate, checks allowed paths and file modes, then runs cloud-patch transport tests, engine tests, ASan/UBSan, and a benchmark smoke test.
- **Publish** uses another fresh runner with a write token. It applies exactly the validated artifact; it never executes candidate build/test code. It refuses stale bases, attributes the commit to Bhinu Santosh <bhinusantosh@gmail.com>, records github-actions[bot] as committer, and pushes normally to main. A no-change result produces no commit. It never force pushes.

The worker cannot change workflows, agent instructions, scripts, Git internals, or automation controls. Only src/, include/, tests/, bench/, docs/, README.md and Makefile can change. It does not ingest public issues or pull requests. Fork copies are blocked by the repository-name guard. Concurrent runs queue under a shared group. Generation times out at 25 minutes, validation at 15, publication at 5. Branch protection must permit the publisher's normal push; otherwise publication fails visibly without weakening protection.

## Pause, failure and evidence

Set `MINICOL_AGENT_ENABLED` to `false` or disable the workflow in GitHub Actions to stop future runs. Cancel a currently running job separately. On failure, inspect the Actions job logs; candidate patches are retained as artifacts for seven days. Never assume a failed run produced a commit. The original benchmark CSV remains the evidence for original resume claims; cloud hardware is different, so reruns need their own machine/methodology record.

The normal correctness workflow is `.github/workflows/ci.yml`. Pushes made with GitHub's built-in token do not normally trigger a second push workflow; the cloud worker therefore performs its own validation before publication.

Sources: [OpenAI Codex action](https://learn.chatgpt.com/docs/github-action), [OpenAI non-interactive authentication](https://learn.chatgpt.com/docs/non-interactive-mode), [GitHub scheduled workflow behavior](https://docs.github.com/en/actions/reference/workflows-and-actions/events-that-trigger-workflows#schedule).
