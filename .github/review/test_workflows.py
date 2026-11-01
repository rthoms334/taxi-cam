"""Structural checks on the review and gate workflows (no YAML library needed)."""

from __future__ import annotations

import re
import unittest
from pathlib import Path

WORKFLOWS = Path(__file__).resolve().parent.parent / "workflows"


def read(name: str) -> str:
    return (WORKFLOWS / name).read_text(encoding="utf-8")


def job(text: str, name: str) -> str:
    """Return one job's block: from `  name:` to the next two-space key."""
    match = re.search(rf"(?ms)^  {re.escape(name)}:\n(.*?)(?=^  [A-Za-z0-9_-]+:\n|\Z)", text)
    if not match:
        raise AssertionError(f"job {name} not found")
    return match.group(1)


def step(text: str, name: str) -> str:
    match = re.search(rf"(?ms)^      - name: {re.escape(name)}\n(.*?)(?=^      - name:|^  [A-Za-z0-9_-]+:\n|\Z)", text)
    if not match:
        raise AssertionError(f"step {name} not found")
    return match.group(1)


def permissions(block: str) -> str:
    match = re.search(r"(?m)^    permissions:(.*)\n((?:      .*\n)*)", block)
    return (match.group(1) + "\n" + match.group(2)) if match else ""


class WorkflowTest(unittest.TestCase):
    def test_every_action_is_pinned_to_a_commit(self):
        for path in WORKFLOWS.glob("*.yml"):
            for ref in re.findall(r"(?m)^\s*(?:-\s*)?uses:\s*(\S+)", path.read_text(encoding="utf-8")):
                if ref.startswith("./"):
                    continue
                self.assertRegex(ref, r"@[0-9a-f]{40}$", f"{path.name}: {ref} must be pinned to a commit SHA")

    def test_only_the_gate_uses_pull_request_target(self):
        for path in WORKFLOWS.glob("*.yml"):
            uses_target = "pull_request_target" in path.read_text(encoding="utf-8")
            self.assertEqual(uses_target, path.name == "pr-risk-gate.yml", path.name)

    def test_no_untrusted_pull_request_text_in_expressions(self):
        for path in WORKFLOWS.glob("*.yml"):
            text = path.read_text(encoding="utf-8")
            for field in ("title", "body", "head.ref", "head.label"):
                self.assertNotIn(f"github.event.pull_request.{field}", text, path.name)

    def test_gate_never_runs_pull_request_code(self):
        gate = read("pr-risk-gate.yml")
        self.assertRegex(gate, r"(?m)^  pull_request_target:\n    branches: \[main\]")
        self.assertNotRegex(gate, r"(?m)^\s+ref:", "the gate checks out main only")
        self.assertNotIn("pull_request.head.sha }}\n", gate.replace("HEAD_SHA: ${{ github.event.pull_request.head.sha }}\n", ""))
        self.assertNotIn("secrets.", gate)
        self.assertNotIn("claude-code-action", gate)
        self.assertRegex(gate, r"(?m)^\s+run: python3 \.github/review/review\.py classify ")
        for name in ("classify", "label", "approval", "live-check", "gate"):
            job(gate, name)

    def test_gate_tiers_and_environments(self):
        gate = read("pr-risk-gate.yml")
        self.assertIn("environment: pr-approval", job(gate, "approval"))
        self.assertIn("environment: pr-live-check", job(gate, "live-check"))
        self.assertIn("name: Risk gate", job(gate, "gate"))
        self.assertIn("needs: [classify, approval, live-check]", job(gate, "gate"))
        self.assertIn("if: ${{ !cancelled() }}", job(gate, "gate"))
        self.assertIn('.user.type == "User"', job(gate, "gate"))
        self.assertIn("required_reviewers", job(gate, "classify"))
        self.assertEqual(permissions(job(gate, "approval")).strip(), "{}")

    def test_reviewer_is_read_only_and_poster_is_separate(self):
        review = read("pr-review.yml")
        self.assertRegex(review, r"(?m)^  pull_request:\n    branches: \[main\]")
        hunt = job(review, "hunt")
        self.assertNotIn("write", permissions(hunt))
        self.assertIn("pull-requests: read", permissions(hunt))
        self.assertIn("github_token: ${{ github.token }}", hunt)
        self.assertNotIn("id-token", review)
        self.assertNotIn("mcp__github", review)
        self.assertNotRegex(hunt, r'"(Write|Edit|Bash)"|Bash\(gh |Bash\(\*')
        self.assertIn("--reviewer-from \"$BASE_SHA\"", hunt)
        post = job(review, "post")
        self.assertIn("name: AI review", post)
        self.assertIn("pull-requests: write", permissions(post))
        self.assertNotIn("claude-code-action", post)
        self.assertNotIn("secrets.", post)

    def test_live_review_and_evals_run_the_same_reviewer(self):
        live = step(read("pr-review.yml"), "Review with Claude")
        evals = step(read("review-evals.yml"), "Review with Claude")
        self.assertEqual(live, evals)

    def test_evals_run_when_the_reviewer_changes(self):
        evals = read("review-evals.yml")
        for path in ("REVIEW.md", ".github/review/prompt.md", ".github/review/findings.schema.json",
                     ".github/review/evals/**", ".github/workflows/pr-review.yml", ".github/workflows/review-evals.yml"):
            self.assertIn(f"      - {path}\n", evals)
        self.assertNotIn("write", permissions(job(evals, "replay")))

    def test_required_check_names_have_one_owner(self):
        # Required checks match by name, so another job with the same name could satisfy one.
        owners = {"PR test build": "pr-test-build.yml", "AI review": "pr-review.yml", "Risk gate": "pr-risk-gate.yml"}
        for path in WORKFLOWS.glob("*.yml"):
            names = re.findall(r"(?m)^    name: (.+?)\s*$", path.read_text(encoding="utf-8"))
            for check, owner in owners.items():
                expected = 1 if path.name == owner else 0
                self.assertEqual(names.count(check), expected, f"{path.name}: job name {check!r}")

    def test_release_build_ignores_review_tooling(self):
        release = read("release.yml")
        self.assertNotIn(".github/review", release)
        self.assertNotIn("REVIEW.md", release)


if __name__ == "__main__":
    unittest.main()
