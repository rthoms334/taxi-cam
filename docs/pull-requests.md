# Pull requests: review, risk and approval

Every pull request to `main` goes through the same path. Automated checks run first. An AI reviewer hunts for defects, verifiers vote on each finding, and the survivors are ranked. Every finding gets an answer. The change's risk then decides which human approvals it needs. When a merged change still breaks something, it becomes an eval case that every later change to the reviewer is scored against.

```text
pull request ─► PR test build ─► AI review ─────────────► findings answered ─► risk gate ─► merge
                (build, validate,  hunt ► vote ► rank      fixed, or resolved     low: none
                 smoke test)       (reviewer from main)    with a reason          medium: owner approval
                                                                                  high: owner approval + live check
merged change breaks something ─► eval case ─► reviewer evals score every reviewer change ─► keep it only if the score holds
```

| Check | Workflow | Runs on |
| --- | --- | --- |
| **PR test build** | [pr-test-build.yml](../.github/workflows/pr-test-build.yml) | Every pull request ([details](releases.md#pull-request-test-builds)) |
| **AI review** | [pr-review.yml](../.github/workflows/pr-review.yml) | Every pull request that is not a draft, and every push to it |
| **Risk gate** | [pr-risk-gate.yml](../.github/workflows/pr-risk-gate.yml) | Every pull request, and every push to it |
| **Score reviewer** | [review-evals.yml](../.github/workflows/review-evals.yml) | Pull requests that change the reviewer, and on demand |

## AI review

The reviewer is Claude, run by [claude-code-action](https://github.com/anthropics/claude-code-action), following [`.github/review/prompt.md`](../.github/review/prompt.md) and the severity rules in [`REVIEW.md`](../REVIEW.md):

1. **Hunt.** Up to six agents search the change in parallel, each in one area: render thread and GPU, simulator memory, lifecycle, the user's system and releases, evidence and tests, and general correctness. Each must trace a concrete failure through the code.
2. **Vote.** Three fresh verifier agents try to refute each candidate. A candidate survives only if at least two confirm it.
3. **Rank.** Survivors are **Important** (introduced by this change), **Pre-existing** (real, but not introduced here) or **Nit** (at most five).

The reviewer has read-only access and returns findings as data. A separate job posts them, so the agent that reads untrusted pull-request text never holds a token that can write. The reviewer's rules and prompt are always taken from `main`, never from the pull request under review. A pull request therefore cannot weaken its own review by editing `REVIEW.md`. The one exception is the pull request that first adds the reviewer, which is reviewed with its own files and says so.

Important findings and nits become review threads on the changed line. When a line is outside the diff, they become file comments instead. A finding outside the diff is anchored to the first changed file. Pre-existing findings appear only in the summary comment, which is updated in place on each push. Each push is reviewed again: the reviewer reads the earlier threads and your replies, and does not repeat a finding already raised. Only replies from people with write access are passed back to it.

### Answering findings

With **Require conversation resolution before merging** turned on (see [setup](#one-time-setup)), every thread blocks the merge until it is resolved:

- **Fix it**, push, and resolve the thread, or
- **reply with the reason** it is not a defect, then resolve the thread.

The **AI review** check fails only when the review could not run (no credential, a timeout, invalid output). Findings never fail it; their unresolved threads block the merge instead.

### Limits

- The reviewer can miss defects and can be wrong. Voting removes most false findings; it does not prove absence.
- Pull-request text is untrusted input. The reviewer is told to report text that addresses it rather than follow it, but prompt injection cannot be ruled out. That is why the reviewer holds no write token, and why no approval depends on it.
- A pull request that edits `pr-review.yml` itself runs its edited copy of that workflow (normal `pull_request` behaviour). Changes under `.github/` are therefore at least medium risk, which needs your approval.
- Pull requests from forks receive no secrets, so the AI review cannot run on them. Review those yourself.

### Cost and model

Each review runs up to six hunters and three verifiers per candidate (at most twelve candidates). The model defaults to `claude-opus-5-5`. Set the repository variable `REVIEW_MODEL` to change it. The run summary shows how many candidates were found and how many the vote dropped.

## Risk tiers

The [risk gate](../.github/workflows/pr-risk-gate.yml) sets a tier from every path a pull request adds, changes, deletes or renames (rules in [`review_tools.py`](../.github/review/review_tools.py), `RISK_RULES`). The highest tier among those paths wins. It labels the pull request `risk: low`, `risk: medium` or `risk: high`, and lists the deciding paths in the run summary.

| Tier | Paths | Approvals needed |
| --- | --- | --- |
| **High** | `src/hooks`, `src/bridge`, `src/graphics`, `src/camera`, `src/shared` (code inside the simulator's process); `src/setup`, `src/app/updater.*`, `installer/` (the user's system); `build.ps1`, `bootstrap.ps1`, `smoke-test.ps1`, `dependencies.json` (the shipped binaries and toolchain) | Owner approval, then a live check |
| **Medium** | Other `src/` code, `tests/`, `changelog.json` and `docs/whats-new/` (installed companions read them from `main`), `.github/`, `ci/`, `REVIEW.md`, `AGENTS.md`, licences, and anything not listed | Owner approval |
| **Low** | Documentation (`docs/`, `*.md`), `tools/` (outside the shipped build), issue forms | None, once the other checks pass and threads are resolved |

The change sets the tier, not its author. The gate runs from `main`'s copy of its workflow (`pull_request_target`), so a pull request cannot change its own tier. The gate never checks out or runs pull-request code.

### Approving

Approvals are GitHub [environment reviews](https://docs.github.com/actions/managing-workflow-runs-and-deployments/managing-deployments/reviewing-deployments). On the pull request, the **Risk gate** run shows **Waiting**. Open it, select **Review deployments**, tick the environment and choose **Approve and deploy**. The gate records who approved each environment and accepts only approvals made by a person.

- **`pr-approval`** (medium and high): you have read the change, its AI review and the answers to every thread.
- **`pr-live-check`** (high only, offered after `pr-approval`): you ran this commit's PR test build in MSFS. Record the result in the pull request's **Live result** section. Local and WARP validation do not establish simulator behaviour, so changes inside the simulator's process need a live check before they merge.

An approval covers one commit. Each push starts a new gate run that needs approving again, and the old run is cancelled.

GitHub records your Claude sessions' pushes, comments and labels under your own account, so a label or `/approve` comment could not show that you approved. An environment review is a separate, deliberate step in GitHub's interface, and [AGENTS.md](../AGENTS.md) forbids agents from attempting one. The live check records your statement that you tested; it does not verify the test.

Required checks are matched by name and source app, not by workflow file. A pull request that adds a workflow with a job named **Risk gate** or **AI review** could therefore satisfy that check. `REVIEW.md` treats such a job as Important, `test_workflows.py` fails when one appears outside its own workflow, and any change under `.github/` needs your approval. Still read every workflow change before approving it.

## Reviewer evals

[`.github/review/evals/cases/`](../.github/review/evals/cases) holds past incidents. Each case is the commit that introduced a real defect and the lines where it sits. The [reviewer evals](../.github/workflows/review-evals.yml) workflow replays the review on each such commit. A case counts as caught when an Important finding lands within five lines of an expected location. The total caught is compared with `caught` in [`baseline.json`](../.github/review/evals/baseline.json).

The workflow runs on any pull request that changes `REVIEW.md`, the prompt, the findings schema, the cases or either review workflow. The candidate reviewer is scored before it can reach `main`, and **Score reviewer** fails if it catches fewer cases than the baseline. When it catches more, raise `caught` in the same pull request. Run it from the Actions tab at any time to check for drift, for example after updating `claude-code-action` or changing `REVIEW_MODEL`.

The baseline starts empty (`null`). After the first run whose result you accept, set `caught` to that number.

The first six cases come from this repository's history:

| Case | Defect |
| --- | --- |
| `snapshot-replan-shared-readback` | Display snapshots replanned copies into one readback buffer and completed on the first copy's fence |
| `tone-table-unfenced-readback` | The tone table was read back with no GPU completion proof |
| `keep-install-exposure-rollback` | A failed install did not roll back the exposure migration, leaving forced settings |
| `named-routing-held-blocks-swap` | A guard stopped panel names from correcting a swapped automatic display pair |
| `named-routing-overrides-user-choice` | Named routing overrode a user's single-display choice and ignored Auto detect off |
| `ini-a380-mips-unrestricted` | Unrestricting the iniBuilds A380's mip count broke its fallback detection |

### Adding a case

When a merged change turns out to have broken something:

1. Find the commit that introduced the defect (`change`), its parent (`base`) and the commit that fixed it (`fix`).
2. Confirm the defect is visible in `git diff base change` and record its lines in the `change` version of each file.
3. Add `.github/review/evals/cases/<id>.json`. The `id` uses lowercase letters, digits and hyphens and matches the file name:

   ```json
   {
     "id": "short-kebab-id",
     "title": "One line",
     "source": "Issue, pull request or commit references",
     "base": "<40-character SHA>",
     "change": "<40-character SHA>",
     "fix": "<40-character SHA>",
     "expected": [{"path": "src/...", "start_line": 10, "end_line": 14, "what": "The defect and its consequence"}],
     "evidence": "Why it is a defect and what the fix changed"
   }
   ```

4. Open a pull request. Adding a case is medium risk, so your approval is the expert check that the case is a real defect. The evals run on that pull request; adjust the baseline if the score changes.

Use defects that can be found by reading the change. Leave out tuning changes, experiments and behaviour that only a live simulator can reveal.

## One-time setup

The repository owner does these steps in GitHub's settings. They are not stored in the repository.

1. **Claude credential.** Under **Settings → Secrets and variables → Actions**, add `ANTHROPIC_API_KEY`, an Anthropic API key billed per use. Alternatively, add `CLAUDE_CODE_OAUTH_TOKEN`, generated with `claude setup-token` from a Claude subscription. That token draws on the subscription's usage limits, which are intended for individual use; an API key is the documented choice for automation. The Claude GitHub app is not needed, because the workflows use their own GitHub token. Optionally add a repository variable `REVIEW_MODEL`.
2. **Approval environments.** Under **Settings → Environments**, create `pr-approval` and `pr-live-check`. For each:
   - **Required reviewers:** yourself.
   - **Prevent self-review:** off. Your Claude sessions push as you, so you are the person who triggered the run.
   - **Deployment branches and tags:** selected branches, `main` only. The gate always runs on `main`.

   Until an environment exists with a required reviewer, the gate refuses the tiers that need it rather than letting the approval pass with no one.
3. **Branch ruleset for `main`.** Under **Settings → Rules → Rulesets**:
   - Require a pull request before merging.
   - Require status checks: **PR test build**, **AI review** and **Risk gate**, each with **GitHub Actions** as its source so that no other app can set it. Do not require **Owner approval** or **Live check**: a job skipped by its condition reports success, so those would pass on low-risk changes. **Risk gate** is the check that enforces them. Do not require **Score reviewer** either: it only runs on pull requests that change the reviewer, and a required check that never runs stays pending. Read its result on those pull requests instead.
   - Require branches to be up to date before merging. GitHub's merge queue is only available to organization-owned repositories, so this setting is what makes the checks run again on the latest `main`.
   - Require conversation resolution before merging.
   - Block force pushes, and leave the bypass list empty. A bypass skips every gate above.
4. **Other review bots (optional).** A review bot such as Codex can keep running alongside as a second opinion. Its threads also need resolving under the conversation-resolution rule.

## After merge

Merging does not ship anything by itself. The [Windows build and release](releases.md) workflow builds a release candidate on `main`, and it is published only after you test it and approve the `release` environment. Only `changelog.json` and `docs/whats-new/` reach installed companions directly from `main`, which is why changes to them are at least medium risk.
