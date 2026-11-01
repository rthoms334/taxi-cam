"""Command-line steps for the review workflows.

    classify    risk tier from the pull request's changed paths
    stage       write the reviewer's inputs (diff, rules, prompt, previous findings)
    save        validate the reviewer's structured output and store it
    post        publish findings as review threads and a summary comment

Standard library only. `post` is the one command that writes to GitHub; it
needs GH_TOKEN with pull-requests: write.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import urllib.error
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import review_tools as rt  # noqa: E402

REVIEWER_FILES = {
    "REVIEW.md": "REVIEW.md",
    ".github/review/prompt.md": "prompt.md",
    ".github/review/findings.schema.json": "findings.schema.json",
}
BOT_LOGIN = "github-actions[bot]"


def git(*args: str) -> str:
    return subprocess.run(["git", "-c", "core.quotePath=false", *args], check=True, capture_output=True, text=True, encoding="utf-8").stdout


def append_output(path: str | None, values: dict[str, str]) -> None:
    if not path:
        return
    with open(path, "a", encoding="utf-8") as handle:
        for key, value in values.items():
            if "\n" in value:
                raise ValueError(f"output {key} must be a single line")
            handle.write(f"{key}={value}\n")


def append_summary(path: str | None, text: str) -> None:
    if path:
        with open(path, "a", encoding="utf-8") as handle:
            handle.write(text)


# --------------------------------------------------------------------------
# classify
# --------------------------------------------------------------------------

def cmd_classify(args: argparse.Namespace) -> int:
    paths = Path(args.files).read_text(encoding="utf-8").splitlines()
    old_names = Path(args.old_names).read_text(encoding="utf-8").splitlines() if args.old_names else []
    result = rt.classify(paths, args.expected, old_names)
    append_output(args.github_output, {"tier": result["tier"]})
    append_summary(args.summary, rt.render_classification(result))
    print(f"risk tier: {result['tier']}")
    return 0


# --------------------------------------------------------------------------
# stage
# --------------------------------------------------------------------------

def load_reviewer(ref: str) -> dict[str, str] | None:
    """Read the reviewer files at a git ref, or None if any is missing."""
    files = {}
    for source, name in REVIEWER_FILES.items():
        try:
            files[name] = git("show", f"{ref}:{source}")
        except subprocess.CalledProcessError:
            return None
    return files


AGENT_FILES = ("AGENTS.md", "CLAUDE.md", "CLAUDE.local.md")


def restore_agent_files(base: str) -> list[str]:
    """Replace agent instruction files in the checkout with main's copies.

    Claude Code loads these files as instructions, so a pull request could
    otherwise tell its own reviewer what to report. Each one is restored from
    `base`, or removed when `base` has none. The pull request's edits to them
    stay in change.diff as material under review.
    """
    def named(paths: str) -> set[str]:
        return {p for p in paths.splitlines() if p.rsplit("/", 1)[-1] in AGENT_FILES}
    at_head = named(git("ls-files"))
    at_base = named(git("ls-tree", "-r", "--name-only", base))
    for path in sorted(at_head | at_base):
        target = Path(path)
        if path in at_base:
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(subprocess.run(["git", "show", f"{base}:{path}"], check=True, capture_output=True).stdout)
        elif target.exists():
            target.unlink()
    return sorted(at_head | at_base)


def load_worktree_reviewer(root: Path) -> dict[str, str]:
    return {name: (root / source).read_text(encoding="utf-8") for source, name in REVIEWER_FILES.items()}


def cmd_stage(args: argparse.Namespace) -> int:
    out = Path(args.out)
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)

    base, head = args.base, args.head
    context = {"base": base, "head": head, "bootstrap": False}
    reviewer = load_reviewer(args.reviewer_from)
    context["reviewer"] = f"`main` at `{args.reviewer_from[:7]}`"
    if reviewer is None:
        if not args.bootstrap_fallback:
            raise SystemExit(f"reviewer files are missing at {args.reviewer_from}")
        reviewer = load_worktree_reviewer(Path.cwd())
        context["reviewer"] = "this pull request"
        context["bootstrap"] = True
    for name, text in reviewer.items():
        (out / name).write_text(text, encoding="utf-8")

    merge_base = git("merge-base", base, head).strip()
    (out / "change.diff").write_text(git("diff", "--no-color", "--no-ext-diff", "-M", merge_base, head), encoding="utf-8")
    (out / "files.txt").write_text(git("diff", "--name-status", "-M", merge_base, head), encoding="utf-8")

    previous: list[dict] = []
    if args.threads:
        threads = json.loads(Path(args.threads).read_text(encoding="utf-8"))
        if not isinstance(threads, list) or not all(isinstance(t, dict) for t in threads):
            raise SystemExit("--threads must be a JSON list of review threads")
        previous = rt.previous_findings(threads)
    (out / "previous.json").write_text(json.dumps(previous, indent=2), encoding="utf-8")

    # After the first review, review only what changed since the last reviewed commit.
    since = None
    if args.summary_comments:
        comments = json.loads(Path(args.summary_comments).read_text(encoding="utf-8"))
        if not isinstance(comments, list) or not all(isinstance(c, dict) for c in comments):
            raise SystemExit("--summary-comments must be a JSON list of issue comments")
        since = rt.last_reviewed_sha(comments)
        if since:
            resolved = subprocess.run(["git", "rev-parse", "--verify", "--quiet", f"{since}^{{commit}}"], capture_output=True, text=True)
            since = resolved.stdout.strip() if resolved.returncode == 0 else None
    context["mode"] = "full"
    if since and subprocess.run(["git", "merge-base", "--is-ancestor", since, head], capture_output=True).returncode == 0:
        context["since"] = since
        pr_files = [p for p in git("diff", "--name-only", merge_base, head).splitlines() if p]
        followup = git("diff", "--no-color", "--no-ext-diff", "-M", since, head, "--", *pr_files) if pr_files else ""
        if followup.strip():
            context["mode"] = "follow-up"
            (out / "followup.diff").write_text(followup, encoding="utf-8")
        else:
            context["mode"] = "skip"
    scope = rt.FOLLOW_UP_SCOPE.format(since=since) if context["mode"] == "follow-up" else rt.FULL_SCOPE
    (out / "scope.md").write_text(scope, encoding="utf-8")
    context["agent_files_from_base"] = restore_agent_files(base)
    (out / "context.json").write_text(json.dumps(context, indent=2), encoding="utf-8")

    schema = json.dumps(json.loads(reviewer["findings.schema.json"]), separators=(",", ":"))
    if "'" in schema:
        raise SystemExit("findings.schema.json must not contain single quotes; it is passed to Claude in single quotes")
    append_output(args.github_output, {"schema": schema, "head": head, "mode": context["mode"]})
    print(f"staged a {context['mode']} review of {head[:7]} against {merge_base[:7]} with the reviewer from {context['reviewer']}")
    return 0


# --------------------------------------------------------------------------
# save
# --------------------------------------------------------------------------

def cmd_save(args: argparse.Namespace) -> int:
    folder = Path(args.out).parent
    context_path = folder / "context.json"
    context = json.loads(context_path.read_text(encoding="utf-8")) if context_path.is_file() else {}
    if context.get("mode") == "skip":
        review = {"summary": "Nothing in this pull request's files changed since the last review.", "findings": [],
                  "stats": {"hunters_run": 0, "candidates": 0, "dropped_by_vote": 0, "dropped_as_repeat": 0}, "complete": True}
        Path(args.out).write_text(json.dumps(review, indent=2), encoding="utf-8")
        print("nothing changed since the last review; saved an empty review without running Claude")
        return 0
    raw = os.environ.get("STRUCTURED_OUTPUT", "")
    secrets = [os.environ.get(name, "") for name in ("ANTHROPIC_API_KEY", "CLAUDE_CODE_OAUTH_TOKEN", "GITHUB_TOKEN")]
    if not raw.strip():
        reason = None
        log = os.environ.get("EXECUTION_FILE") or os.path.join(os.environ.get("RUNNER_TEMP", ""), "claude-execution-output.json")
        try:
            reason = rt.execution_error(json.loads(Path(log).read_text(encoding="utf-8")), secrets)
        except (OSError, ValueError):
            pass
        print(f"::error::The reviewer returned no structured output. Claude Code's last message: {reason or 'not available'}")
        return 1
    leaked = rt.find_credentials(raw, secrets)
    if leaked:
        print(f"::error::The reviewer's output contains a credential ({', '.join(sorted(set(leaked)))}); it was not saved or posted. Rotate the credential if it may have been exposed elsewhere.")
        return 1
    try:
        review = rt.validate_review(json.loads(raw))
    except (json.JSONDecodeError, rt.FindingsError) as error:
        print(f"::error::The reviewer's output is invalid: {error}")
        return 1
    if not review["complete"]:
        print(f"::error::The review did not finish: not every hunter or verifier returned. Re-run the job. Reviewer's summary: {review['summary'][:400]}")
        return 1
    if context.get("mode") == "follow-up":
        lines = rt.commentable_lines((folder / "followup.diff").read_text(encoding="utf-8"), context=False)
        review["findings"], dropped = rt.follow_up_scope(review["findings"], lines)
        context["scope_dropped"] = len(dropped)
        context_path.write_text(json.dumps(context, indent=2), encoding="utf-8")
        if dropped:
            print(f"dropped {len(dropped)} finding(s) outside the follow-up scope")
    Path(args.out).write_text(json.dumps(review, indent=2), encoding="utf-8")
    counts = {s: sum(1 for f in review["findings"] if f["severity"] == s) for s in rt.SEVERITIES}
    print(f"saved {len(review['findings'])} findings: {counts}")
    return 0


# --------------------------------------------------------------------------
# post
# --------------------------------------------------------------------------

class GitHub:
    def __init__(self, token: str, repo: str) -> None:
        self.token = token
        self.repo = repo
        self.api = os.environ.get("GITHUB_API_URL", "https://api.github.com").rstrip("/")

    def request(self, method: str, path: str, body: dict | None = None) -> tuple[object, dict]:
        url = path if path.startswith("http") else f"{self.api}{path}"
        data = json.dumps(body).encode("utf-8") if body is not None else None
        request = urllib.request.Request(url, data=data, method=method)
        request.add_header("Authorization", f"Bearer {self.token}")
        request.add_header("Accept", "application/vnd.github+json")
        request.add_header("X-GitHub-Api-Version", "2022-11-28")
        if data is not None:
            request.add_header("Content-Type", "application/json")
        with urllib.request.urlopen(request, timeout=60) as response:
            payload = response.read()
            return (json.loads(payload) if payload else None), dict(response.headers)

    def review_threads(self, number: int) -> list[dict]:
        owner, name = self.repo.split("/", 1)
        query = """query($owner: String!, $name: String!, $number: Int!, $cursor: String) {
  repository(owner: $owner, name: $name) { pullRequest(number: $number) {
    reviewThreads(first: 100, after: $cursor) {
      pageInfo { hasNextPage endCursor }
      nodes { isResolved comments(first: 1) { nodes { body author { login } } } } } } } }"""
        threads: list[dict] = []
        cursor = None
        while True:
            data, _ = self.request("POST", "/graphql", {"query": query, "variables": {"owner": owner, "name": name, "number": number, "cursor": cursor}})
            if not isinstance(data, dict) or data.get("errors") or not (data.get("data") or {}).get("repository"):
                raise RuntimeError(f"review thread query failed: {data.get('errors') if isinstance(data, dict) else data}")
            page = data["data"]["repository"]["pullRequest"]["reviewThreads"]
            threads.extend(page["nodes"])
            if not page["pageInfo"]["hasNextPage"]:
                return threads
            cursor = page["pageInfo"]["endCursor"]

    def paginate(self, path: str) -> list:
        items: list = []
        url: str | None = f"{path}{'&' if '?' in path else '?'}per_page=100"
        while url:
            page, headers = self.request("GET", url)
            items.extend(page or [])
            url = None
            for part in (headers.get("Link") or headers.get("link") or "").split(","):
                if 'rel="next"' in part:
                    url = part.split(";")[0].strip().strip("<>")
        return items


def cmd_post(args: argparse.Namespace) -> int:
    folder = Path(args.dir)
    raw = (folder / "findings.json").read_text(encoding="utf-8")
    if rt.find_credentials(raw, []):
        print("::error::The findings contain something shaped like a credential; nothing was posted.")
        return 1
    review = rt.validate_review(json.loads(raw))
    context = json.loads((folder / "context.json").read_text(encoding="utf-8"))
    lines = rt.commentable_lines((folder / "change.diff").read_text(encoding="utf-8"))
    head = context["head"]
    gh = GitHub(os.environ["GH_TOKEN"], args.repo)
    pr = f"/repos/{args.repo}/pulls/{args.pr}"

    open_threads = rt.open_thread_fingerprints(gh.review_threads(args.pr))
    placements, repeats = rt.plan_placements(review["findings"], lines, open_threads)

    inline = [p for p in placements if p.kind == "line"]
    if inline:
        counts = {s: sum(1 for p in placements if p.finding["severity"] == s) for s in rt.SEVERITIES}
        try:
            gh.request("POST", f"{pr}/reviews", {
                "commit_id": head,
                "event": "COMMENT",
                "body": f"AI review of `{head[:7]}`: {counts['important']} important, {counts['nit']} nit. The summary comment lists every finding.",
                "comments": [{"path": p.path, "line": p.line, "side": "RIGHT", "body": rt.render_inline(p, head)} for p in inline],
            })
        except urllib.error.HTTPError as error:
            if error.code != 422:
                raise
            print("::warning::GitHub refused the batched review; posting each finding separately.")
            for placement in inline:
                try:
                    gh.request("POST", f"{pr}/comments", {"commit_id": head, "path": placement.path, "line": placement.line, "side": "RIGHT", "body": rt.render_inline(placement, head)})
                except urllib.error.HTTPError as single:
                    if single.code != 422:
                        raise
                    placement.kind = "file"

    for placement in [p for p in placements if p.kind == "file"]:
        if placement.path is None:
            placement.path = placement.finding["path"]
        try:
            gh.request("POST", f"{pr}/comments", {"commit_id": head, "path": placement.path, "subject_type": "file", "body": rt.render_inline(placement, head)})
        except urllib.error.HTTPError as error:
            if error.code != 422:
                raise
            print(f"::warning::GitHub refused a file comment on {placement.path}; the finding stays in the summary.")
            placement.kind = "summary"

    summary = rt.render_summary(review, placements, repeats, context, args.run_url)
    existing = [c for c in gh.paginate(f"/repos/{args.repo}/issues/{args.pr}/comments")
                if (c.get("user") or {}).get("login") == BOT_LOGIN and rt.SUMMARY_MARKER in (c.get("body") or "")]
    if existing:
        gh.request("PATCH", f"/repos/{args.repo}/issues/comments/{existing[-1]['id']}", {"body": summary})
    else:
        gh.request("POST", f"/repos/{args.repo}/issues/{args.pr}/comments", {"body": summary})
    append_summary(args.summary, summary.replace(rt.SUMMARY_MARKER + "\n", ""))
    print(f"posted {len(placements)} findings ({len(inline)} inline); {len(repeats)} repeats skipped")
    return 0



def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("classify")
    p.add_argument("--files", required=True, help="file with one changed path per line")
    p.add_argument("--old-names", help="file with the previous name of each renamed file")
    p.add_argument("--expected", type=int, help="GitHub's changed-file count")
    p.add_argument("--github-output")
    p.add_argument("--summary")
    p.set_defaults(func=cmd_classify)

    p = sub.add_parser("stage")
    p.add_argument("--out", required=True)
    p.add_argument("--base", required=True)
    p.add_argument("--head", required=True)
    p.add_argument("--reviewer-from", required=True, help="git ref holding the reviewer files")
    p.add_argument("--bootstrap-fallback", action="store_true", help="use the working tree's reviewer when the ref has none")
    p.add_argument("--threads", help="the pull request's review threads (GraphQL nodes) as JSON")
    p.add_argument("--summary-comments", help="the pull request's issue comments as JSON, to find the last reviewed commit")
    p.add_argument("--github-output")
    p.set_defaults(func=cmd_stage)

    p = sub.add_parser("save")
    p.add_argument("--out", required=True)
    p.set_defaults(func=cmd_save)

    p = sub.add_parser("post")
    p.add_argument("--dir", required=True)
    p.add_argument("--repo", required=True)
    p.add_argument("--pr", required=True, type=int)
    p.add_argument("--run-url", required=True)
    p.add_argument("--summary")
    p.set_defaults(func=cmd_post)


    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
