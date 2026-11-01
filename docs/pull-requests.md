# Pull requests: review, risk and approval

Every pull request to `main` goes through the same path. Automated checks run first. An AI reviewer hunts for defects, verifiers vote on each finding, and the survivors are ranked. Every finding gets an answer. The change's risk then decides which human approvals it needs.

```text
pull request ─► PR test build ─► AI review ─────────────► findings answered ─► risk gate ─► merge
                (build, validate,  hunt ► vote ► rank      fixed, or resolved     low: none
                 smoke test)       (reviewer from main)    with a reason          medium: owner approval
                                                                                  high: owner approval + live check
```

| Check | Workflow | Runs on |
| --- | --- | --- |
| **PR test build** | [pr-test-build.yml](../.github/workflows/pr-test-build.yml) | Every pull request ([details](releases.md#pull-request-test-builds)) |
| **AI review** | [pr-review.yml](../.github/workflows/pr-review.yml) | Every pull request that is not a draft, and every push to it |
| **Risk gate** | [pr-risk-gate.yml](../.github/workflows/pr-risk-gate.yml) | Every pull request, and every push to it |

## AI review

The reviewer is Claude, run by [claude-code-action](https://github.com/anthropics/claude-code-action), following [`.github/review/prompt.md`](../.github/review/prompt.md) and the severity rules in [`REVIEW.md`](../REVIEW.md):

1. **Hunt.** Up to six agents search the change in parallel, each in one area: render thread and GPU, simulator memory, lifecycle, the user's system and releases, evidence and tests, and general correctness. Each must trace a concrete failure through the code.
2. **Vote.** Three fresh verifier agents try to refute each candidate. A candidate survives only if at least two confirm it.
3. **Rank.** Survivors are **Important** (introduced by this change), **Pre-existing** (real, but not introduced here) or **Nit** (at most five).

The reviewer has read-only access and returns findings as data. A separate job posts them, so the agent that reads untrusted pull-request text never holds a token that can write. The reviewer's rules and prompt (`REVIEW.md`, `prompt.md` and the findings schema) are taken from `main`, never from the pull request under review, and every `AGENTS.md`, `CLAUDE.md` and `CLAUDE.local.md` in the checkout is replaced with `main`'s copy before Claude starts. A pull request therefore cannot weaken its own review by editing them; its edits to those files are reviewed as part of the diff. The one exception is the pull request that first adds the reviewer, which is reviewed with its own files and says so. The workflow and the tooling that stages, checks and posts the review (`.github/review/*.py`) do run as the pull request edits them; see [Limits](#limits).

Important findings and nits become review threads on the changed line. When a line is outside the diff, they become file comments instead, including on deleted files. An Important finding in a file outside the diff is anchored to the first changed file; a nit there appears only in the summary. Pre-existing findings appear only in the summary comment, which is updated in place on each push. The first review covers the whole pull request. Later pushes get a **follow-up review** of only what changed since the last reviewed commit, which the summary comment records:

- The reviewer runs at most two hunters and reports only Important findings on those lines, including an earlier finding that a fix left in place. It reports no nits and no pre-existing findings.
- The validation step removes anything outside that scope before posting, so fixing findings does not open review of code that was already reviewed. The summary says how many findings were removed.
- Follow-up lines are the pull request's own changes that are new since the last review, including the lines on either side of a deletion. Changes that arrive by merging `main` are left out. When nothing is left (for example, after only merging `main`), Claude is not run at all.
- A force push that rewrites the reviewed commit out of the branch gets a full review again.

The reviewer reads the earlier threads, whether each was resolved, and the replies from people with write access. It drops a finding that a reply explains is not a defect. A finding whose thread is still open is not posted twice. If you resolve a thread but the reviewer still finds the defect on a later push, it opens a new thread.

### Answering findings

With **Require conversation resolution before merging** turned on (see [setup](#one-time-setup)), every thread blocks the merge until it is resolved:

- **Fix it**, push, and resolve the thread, or
- **reply with the reason** it is not a defect, then resolve the thread.

The **AI review** check fails only when the review could not run (no credential, a timeout, invalid output). Findings never fail it; their unresolved threads block the merge instead.

### Limits

- The reviewer can miss defects and can be wrong. Voting removes most false findings; it does not prove absence.
- Pull-request text is untrusted input. The reviewer is told to report text that addresses it rather than follow it, but prompt injection cannot be ruled out. So the reviewer holds no write token, and no approval depends on it. It may only read files and run `git log` and `git show`; it cannot read `/proc`, `/sys` or `.git/config`, and secrets are scrubbed from the commands it runs. Its output is refused, and nothing is posted, if it contains the job's Claude credential or GitHub token, or anything shaped like a key or token.
- A pull request that edits `pr-review.yml` or `.github/review/*.py` runs its edited copies (normal `pull_request` behaviour), including the steps that check the reviewer's output for credentials and completeness. Changes under `.github/` are therefore at least medium risk, which needs your approval: read them before approving.
- Pull requests from forks receive no secrets, so the AI review cannot run on them and its required check fails. To take a contribution from a fork, review it yourself, push its commits to a branch in this repository and open a pull request from there; the review and the gate then run normally.

### Cost and model

A full review runs up to six hunters and three verifiers per candidate (at most twelve candidates); a follow-up runs at most two hunters and four candidates. The model defaults to `claude-opus-5-5`. Set the repository variable `REVIEW_MODEL` to change it. The run summary shows how many candidates were found and how many the vote dropped.

## Risk tiers

The [risk gate](../.github/workflows/pr-risk-gate.yml) sets a tier from every path a pull request adds, changes, deletes or renames (rules in [`review_tools.py`](../.github/review/review_tools.py), `RISK_RULES`). The highest tier among those paths wins. It labels the pull request `risk: low`, `risk: medium` or `risk: high`, and lists the deciding paths in the run summary.

| Tier | Paths | Approvals needed |
| --- | --- | --- |
| **High** | `src/hooks`, `src/bridge`, `src/graphics`, `src/camera`, `src/shared`, `src/profiles` and `tools/diagnostics/image_inventory.*` (code compiled into the bridge, inside the simulator's process); `src/app/launcher.*` (writes into the simulator and starts the bridge there); `src/setup`, `src/app/updater.*`, `src/app/json_value.hpp`, `installer/` (the user's system); `build.ps1`, `bootstrap.ps1`, `smoke-test.ps1`, `dependencies.json` (the shipped binaries and toolchain) | Owner approval, then a live check |
| **Medium** | The rest of `src/app/`, `tests/`, `changelog.json` and `docs/whats-new/` (installed companions read them from `main`), `.github/`, `ci/`, `REVIEW.md`, `AGENTS.md`, licences, and anything not listed | Owner approval |
| **Low** | Documentation (`docs/`, `*.md`), the rest of `tools/` (outside the shipped build), issue forms | None, once the other checks pass and threads are resolved |

The change sets the tier, not its author. The gate runs from `main`'s copy of its workflow (`pull_request_target`), so a pull request cannot change its own tier. The gate never checks out or runs pull-request code.

### Approving

Approvals are GitHub [environment reviews](https://docs.github.com/actions/managing-workflow-runs-and-deployments/managing-deployments/reviewing-deployments). On the pull request, the **Risk gate** run shows **Waiting**. Open it, select **Review deployments**, tick the environment and choose **Approve and deploy**. The gate records who approved each environment and accepts only approvals made by a person.

- **`pr-approval`** (medium and high): you have read the change, its AI review and the answers to every thread.
- **`pr-live-check`** (high only, offered after `pr-approval`): you ran this commit's PR test build in MSFS. Record the result in the pull request's **Live result** section. Local and WARP validation do not establish simulator behaviour, so changes inside the simulator's process need a live check before they merge.

An approval covers one commit. Each push starts a new gate run that needs approving again, and the old run is cancelled.

GitHub records your Claude sessions' pushes, comments and labels under your own account, so a label or `/approve` comment could not show that you approved. An environment review is a separate, deliberate step in GitHub's interface, and [AGENTS.md](../AGENTS.md) forbids agents from attempting one. The live check records your statement that you tested; it does not verify the test.

**Unmitigated limit: required checks can be spoofed by a pull request.** Checks are matched by name and source app, not by workflow file, and on a repository owned by a personal account GitHub offers no rule that pins a check to a workflow on `main`. A pull request that adds a workflow with a job named **Risk gate** (or one named by an expression that evaluates to it) could satisfy the check without any approval. `test_workflows.py` and `REVIEW.md` only catch this by accident: the test runs from the pull request and can be edited by it. The real gate is the **Risk gate** run triggered by `pull_request_target`; it waits for your approval and is listed separately on the pull request. Before merging, check that this run passed, read every change under `.github/workflows/` yourself, and do not merge a pull request that shows two **Risk gate** checks.

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
   - Require status checks: **PR test build**, **AI review** and **Risk gate**, each with **GitHub Actions** as its source so that no other app can set it. Do not require **Owner approval** or **Live check**: a job skipped by its condition reports success, so those would pass on low-risk changes. **Risk gate** is the check that enforces them.
   - GitHub lists a check here only after it has run in the repository. **AI review** appears after its first run on any pull request. **Risk gate** runs from `main`, so it appears only after this workflow is merged and a later pull request has run it. Add it then: requiring it earlier blocks the pull request that adds it, because the gate cannot run there.
   - Require branches to be up to date before merging. GitHub's merge queue is only available to organization-owned repositories, so this setting is what makes the checks run again on the latest `main`.
   - Require conversation resolution before merging.
   - Block force pushes, and leave the bypass list empty. A bypass skips every gate above.
4. **Other review bots (optional).** A review bot such as Codex can keep running alongside as a second opinion. Its threads also need resolving under the conversation-resolution rule.

## After merge

Merging does not ship anything by itself. The [Windows build and release](releases.md) workflow builds a release candidate on `main`, and it is published only after you test it and approve the `release` environment. Only `changelog.json` and `docs/whats-new/` reach installed companions directly from `main`, which is why changes to them are at least medium risk.
