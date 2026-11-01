"""Pure helpers for the pull-request review pipeline.

Everything here is deterministic and offline: risk classification, diff
parsing, findings validation, comment rendering and eval scoring. Network
calls live in review.py. Standard library only, Python 3.10 or newer.
"""

from __future__ import annotations

import codecs
import hashlib
import re
from dataclasses import dataclass
from fnmatch import fnmatchcase

# --------------------------------------------------------------------------
# Risk tiers
# --------------------------------------------------------------------------

TIERS = ("low", "medium", "high")

APPROVALS = {
    "low": "No human approval. Merge once the PR test build and AI review pass and every review conversation is resolved.",
    "medium": "Owner approval of this exact commit (environment `pr-approval`).",
    "high": "Owner approval of this exact commit (`pr-approval`), then a live check of this commit's PR test build in MSFS (`pr-live-check`).",
}

# Ordered: the first rule matching a path sets that path's tier. A pull
# request takes the highest tier of any path it adds, changes, deletes or
# renames. `*` also matches `/`, so `src/hooks/*` covers nested files.
RISK_RULES: tuple[tuple[str, str, str], ...] = (
    ("high", "src/hooks/*", "hooks and the Win64 thunk run inside the simulator"),
    ("high", "src/bridge/*", "the bridge DLL runs inside the simulator"),
    ("high", "src/graphics/*", "GPU capture, composition and synchronization run inside the simulator"),
    ("high", "src/camera/*", "simulator memory, camera ownership and lifecycle"),
    ("high", "src/shared/*", "compiled into the in-process bridge: protocol, locks and control policy"),
    ("high", "src/profiles/*", "aircraft profiles compiled into the in-process bridge"),
    ("high", "tools/diagnostics/image_inventory.*", "compiled into the shipped bridge DLL"),
    ("high", "src/setup/*", "installer commands change exe.xml and saved settings"),
    ("high", "src/app/updater.*", "downloads and installs updates"),
    ("high", "installer/*", "Setup and uninstall change the user's system"),
    ("high", "build.ps1", "builds and validates the shipped binaries"),
    ("high", "bootstrap.ps1", "downloads and verifies the toolchain"),
    ("high", "smoke-test.ps1", "validates the exact shipped binaries"),
    ("high", "dependencies.json", "pins the compiler and installer toolchain"),
    ("low", ".github/ISSUE_TEMPLATE/*", "issue forms"),
    ("medium", ".github/*", "CI, review and merge gates"),
    ("medium", "ci/*", "release versioning and publication"),
    ("medium", "REVIEW.md", "rules for the reviewer"),
    ("medium", "AGENTS.md", "rules for coding agents"),
    ("medium", "CLAUDE.md", "rules for coding agents"),
    ("medium", "changelog.json", "the release version; installed companions read it from main"),
    ("medium", "docs/whats-new/*", "installed companions fetch these pictures from main"),
    ("medium", "LICENSE", "shipped licence"),
    ("medium", "THIRD_PARTY_NOTICES.md", "shipped notices"),
    ("medium", "licenses/*", "shipped notices"),
    ("low", "docs/*", "documentation"),
    ("low", "tools/*", "development tools outside the shipped build"),
    ("low", "*.md", "documentation"),
)
DEFAULT_RISK = ("medium", "not covered by a lower-risk rule")


def classify_path(path: str) -> tuple[str, str]:
    """Return (tier, reason) for one repository-relative path."""
    for tier, pattern, reason in RISK_RULES:
        if fnmatchcase(path, pattern):
            return tier, reason
    return DEFAULT_RISK


def classify(paths: list[str], expected_count: int | None = None) -> dict:
    """Classify a pull request from every path it touches.

    `expected_count` is GitHub's changed-file count for the pull request. The
    files endpoint stops listing at 3000 files, so a shorter list means the
    classifier cannot see every path and the change is treated as high risk.
    """
    unique = sorted({p.strip() for p in paths if p.strip()})
    per_path = []
    for path in unique:
        tier, reason = classify_path(path)
        per_path.append({"path": path, "tier": tier, "reason": reason})

    notes = []
    tier = "low"
    if not unique:
        tier = "medium"
        notes.append("No changed files were listed.")
    for item in per_path:
        if TIERS.index(item["tier"]) > TIERS.index(tier):
            tier = item["tier"]
    if expected_count is not None and len(unique) < expected_count:
        # Renames add their old path, so more paths than files is normal.
        tier = "high"
        notes.append(f"GitHub reports {expected_count} changed files but listed {len(unique)}; the file list is incomplete.")
    return {"tier": tier, "paths": per_path, "notes": notes}


def render_classification(result: dict) -> str:
    tier = result["tier"]
    lines = [f"## Risk: {tier}", "", APPROVALS[tier], ""]
    lines += [f"- {note}" for note in result["notes"]]
    deciding = [p for p in result["paths"] if p["tier"] == tier]
    if deciding:
        lines += ["", f"Paths that set the {tier} tier:", ""]
        for item in deciding[:30]:
            lines.append(f"- `{item['path']}`: {item['reason']}")
        if len(deciding) > 30:
            lines.append(f"- and {len(deciding) - 30} more")
    return "\n".join(lines) + "\n"


# --------------------------------------------------------------------------
# Diff parsing
# --------------------------------------------------------------------------

_HUNK = re.compile(r"^@@ -\d+(?:,(\d+))? \+(\d+)(?:,(\d+))? @@")


def _diff_path(token: str) -> str | None:
    """Turn a `a/x`, `b/x`, quoted path or /dev/null into a repository path."""
    token = token.strip()
    if token == "/dev/null":
        return None
    if token.startswith('"') and token.endswith('"'):
        raw = codecs.escape_decode(token[1:-1].encode("utf-8"))[0]
        token = raw.decode("utf-8", errors="replace")
    if token.startswith(("a/", "b/")):
        token = token[2:]
    return token


def commentable_lines(diff: str) -> dict[str, set[int]]:
    """Map each path present at head to the head-side lines GitHub accepts.

    GitHub accepts review comments on added and context lines inside a hunk
    (side RIGHT). Files changed without hunks (binary, mode-only, pure
    renames) map to an empty set: they accept file-level comments only.
    Deleted files are left out.
    """
    files: dict[str, set[int]] = {}
    header_path: str | None = None
    deleted = False
    current: str | None = None
    old_left = new_left = 0
    new_line = 0

    def close_file() -> None:
        if header_path is not None and not deleted:
            files.setdefault(header_path, set())

    for raw in diff.splitlines():
        if old_left > 0 or new_left > 0:
            marker = raw[:1]
            if marker == "\\":
                continue
            if marker == "+":
                if current is not None:
                    files.setdefault(current, set()).add(new_line)
                new_line += 1
                new_left -= 1
            elif marker == "-":
                old_left -= 1
            else:  # context line; an empty line is context with its space trimmed
                if current is not None:
                    files.setdefault(current, set()).add(new_line)
                new_line += 1
                new_left -= 1
                old_left -= 1
            continue

        if raw.startswith("diff --git "):
            close_file()
            header_path, deleted, current = None, False, None
            rest = raw[len("diff --git "):]
            if rest.startswith('"') or " b/" not in rest:
                parts = rest.rsplit(" ", 1)
                header_path = _diff_path(parts[-1])
            else:
                header_path = _diff_path("b/" + rest.split(" b/", 1)[1])
        elif raw.startswith("deleted file mode"):
            deleted = True
        elif raw.startswith("rename to "):
            header_path = raw[len("rename to "):]
        elif raw.startswith("+++ "):
            current = _diff_path(raw[4:])
            if current is not None:
                header_path = current
        elif raw.startswith("@@ "):
            match = _HUNK.match(raw)
            if match:
                old_left = int(match.group(1)) if match.group(1) is not None else 1
                new_line = int(match.group(2))
                new_left = int(match.group(3)) if match.group(3) is not None else 1
    close_file()
    return files


# --------------------------------------------------------------------------
# Findings
# --------------------------------------------------------------------------

SEVERITIES = ("important", "pre-existing", "nit")
SEVERITY_LABEL = {"important": "Important", "pre-existing": "Pre-existing", "nit": "Nit"}
INLINE_MARKER = "taxi-cam-ai-review"
SUMMARY_MARKER = "<!-- taxi-cam-ai-review-summary -->"
_FP = re.compile(r"<!-- " + INLINE_MARKER + r" fp=([0-9a-f]{12})")


class FindingsError(ValueError):
    pass


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise FindingsError(message)


def validate_review(data: object) -> dict:
    """Check the reviewer's structured output and return a normalized copy."""
    _require(isinstance(data, dict), "review output must be an object")
    summary = data.get("summary")
    findings = data.get("findings")
    stats = data.get("stats")
    _require(isinstance(summary, str), "summary must be a string")
    _require(isinstance(findings, list), "findings must be a list")
    _require(len(findings) <= 25, "at most 25 findings")
    _require(isinstance(stats, dict), "stats must be an object")
    clean_stats = {}
    for key in ("hunters_run", "candidates", "dropped_by_vote", "dropped_as_repeat"):
        value = stats.get(key)
        _require(isinstance(value, int) and not isinstance(value, bool) and value >= 0, f"stats.{key} must be a non-negative integer")
        clean_stats[key] = value

    clean = []
    for index, item in enumerate(findings):
        where = f"findings[{index}]"
        _require(isinstance(item, dict), f"{where} must be an object")
        severity = item.get("severity")
        _require(severity in SEVERITIES, f"{where}.severity must be one of {', '.join(SEVERITIES)}")
        path = item.get("path")
        _require(isinstance(path, str) and path.strip(), f"{where}.path must be a non-empty string")
        path = path.strip()
        while path.startswith("./"):
            path = path[2:]
        _require(not path.startswith("/") and ".." not in path.split("/") and "\n" not in path, f"{where}.path must be repository-relative")
        line = item.get("line")
        _require(isinstance(line, int) and not isinstance(line, bool) and line >= 0, f"{where}.line must be a non-negative integer")
        title = item.get("title")
        _require(isinstance(title, str) and title.strip(), f"{where}.title must be a non-empty string")
        body = item.get("body")
        _require(isinstance(body, str) and body.strip(), f"{where}.body must be a non-empty string")
        votes = item.get("votes_confirmed")
        _require(isinstance(votes, int) and not isinstance(votes, bool) and 2 <= votes <= 3, f"{where}.votes_confirmed must be 2 or 3")
        clean.append({
            "severity": severity,
            "path": path,
            "line": line,
            "title": " ".join(title.split())[:120],
            "body": body.strip()[:4000],
            "votes_confirmed": votes,
        })
    clean.sort(key=lambda f: SEVERITIES.index(f["severity"]))  # stable: keeps the reviewer's order within a severity
    return {"summary": summary.strip(), "findings": clean, "stats": clean_stats}


def fingerprint(finding: dict) -> str:
    """Identify a finding across pushes by severity, file and title."""
    words = re.findall(r"[a-z0-9]+", finding["title"].lower())
    key = f"{finding['severity']}|{finding['path']}|{' '.join(words)}"
    return hashlib.sha1(key.encode("utf-8")).hexdigest()[:12]


BOT_LOGINS = ("github-actions[bot]", "github-actions")  # REST and GraphQL spellings


def open_thread_fingerprints(threads: list[dict]) -> set[str]:
    """Fingerprints of the reviewer's threads that are still unresolved.

    `threads` are GraphQL reviewThreads nodes. A finding with an open thread
    is not posted again. A resolved thread does not suppress a finding: if
    the reviewer still reports it, the defect needs answering again.
    """
    found = set()
    for thread in threads:
        if thread.get("isResolved"):
            continue
        first = ((thread.get("comments") or {}).get("nodes") or [{}])[0]
        if ((first.get("author") or {}).get("login")) in BOT_LOGINS:
            found.update(_FP.findall(first.get("body") or ""))
    return found


_CREDENTIAL_PATTERNS = (
    ("Anthropic key", re.compile(r"sk-ant-[A-Za-z0-9_-]{10,}")),
    ("GitHub token", re.compile(r"\bgh[opsu]_[A-Za-z0-9]{20,}")),
    ("GitHub token", re.compile(r"\bgithub_pat_[A-Za-z0-9_]{20,}")),
    ("token in a URL", re.compile(r"x-access-token:[^@\s]+@")),
)


def find_credentials(text: str, secrets: list[str]) -> list[str]:
    """Name the credentials found in text, without repeating them.

    The reviewer reads untrusted text while its process holds credentials, so
    its output is refused if it contains one of the job's secrets or anything
    shaped like a key or token.
    """
    found = [f"secret #{index + 1}" for index, secret in enumerate(secrets) if secret and len(secret) >= 8 and secret in text]
    found += [name for name, pattern in _CREDENTIAL_PATTERNS if pattern.search(text)]
    return found


def execution_error(messages: object, secrets: list[str]) -> str | None:
    """Claude Code's final message from claude-code-action's execution log.

    The action hides Claude's output, so a run that ends without structured
    output (an authentication error, an unavailable model, a turn limit)
    would otherwise fail with no reason. The text is shortened and withheld
    if it contains anything that looks like a credential.
    """
    if not isinstance(messages, list):
        return None
    text = None
    for message in reversed(messages):
        if isinstance(message, dict) and message.get("type") == "result":
            value = message.get("result") or message.get("error") or message.get("subtype")
            text = value if isinstance(value, str) else None
            break
    if not text:
        return None
    text = " ".join(text.split())[:400]
    if find_credentials(text, secrets):
        return "(withheld: the message contains something shaped like a credential)"
    return text


TRUSTED_ASSOCIATIONS = ("OWNER", "MEMBER", "COLLABORATOR")


def previous_findings(comments: list[dict], bot_login: str) -> list[dict]:
    """Summarize posted findings and the replies of people with write access.

    Replies from anyone else are left out: on a public repository anyone can
    reply, and the reviewer must not read their text as context.
    """
    ours = [c for c in comments if (c.get("user") or {}).get("login") == bot_login and _FP.search(c.get("body") or "")]
    replies: dict[int, list[dict]] = {}
    for comment in comments:
        parent = comment.get("in_reply_to_id")
        if parent is None or comment.get("author_association") not in TRUSTED_ASSOCIATIONS:
            continue
        replies.setdefault(parent, []).append({
            "author": (comment.get("user") or {}).get("login", ""),
            "body": (comment.get("body") or "")[:2000],
        })
    result = []
    for comment in ours:
        body = comment.get("body") or ""
        text = body.split("<sub>", 1)[0].strip()
        result.append({
            "path": comment.get("path"),
            "line": comment.get("line") or comment.get("original_line") or 0,
            "finding": text[:3000],
            "replies": replies.get(comment.get("id"), []),
        })
    return result


@dataclass
class Placement:
    finding: dict
    kind: str  # "line", "file" or "summary"
    path: str | None = None
    line: int | None = None
    note: str = ""


def plan_placements(findings: list[dict], lines: dict[str, set[int]], seen: set[str]) -> tuple[list[Placement], list[dict]]:
    """Decide where each finding is posted.

    Inline threads block merging until resolved (with conversation resolution
    required), so Important findings always get a thread: on their line when
    GitHub accepts it, otherwise as a file comment, and as a file comment on
    the first changed file when their own file is not in the diff. Nits get a
    thread when their file is in the diff. Pre-existing findings stay in the
    summary because they do not need an answer on this pull request.
    Findings whose thread from an earlier push is still open are returned as
    repeats.
    """
    placements: list[Placement] = []
    repeats: list[dict] = []
    anchor = next(iter(sorted(lines)), None)
    for finding in findings:
        if fingerprint(finding) in seen:
            repeats.append(finding)
            continue
        path, line = finding["path"], finding["line"]
        location = f"`{path}:{line}`" if line else f"`{path}`"
        if finding["severity"] == "pre-existing":
            placements.append(Placement(finding, "summary"))
        elif path in lines and line in lines[path]:
            placements.append(Placement(finding, "line", path, line))
        elif path in lines:
            note = f"Line {line} is outside the diff." if line else ""
            placements.append(Placement(finding, "file", path, None, note))
        elif finding["severity"] == "important" and anchor is not None:
            placements.append(Placement(finding, "file", anchor, None, f"Location: {location} (not part of this diff)."))
        else:
            placements.append(Placement(finding, "summary"))
    return placements, repeats


def render_inline(placement: Placement, head_sha: str) -> str:
    finding = placement.finding
    parts = [f"**{SEVERITY_LABEL[finding['severity']]}:** {finding['title']}", ""]
    if placement.note:
        parts += [placement.note, ""]
    parts += [finding["body"], ""]
    answer = "Fix it, or reply with the reason and resolve this conversation."
    parts.append(f"<sub>AI review of `{head_sha[:7]}` · {finding['votes_confirmed']} of 3 verifiers confirmed · {answer}</sub>")
    parts.append(f"<!-- {INLINE_MARKER} fp={fingerprint(finding)} -->")
    return "\n".join(parts)


def _escape_cell(text: str) -> str:
    return text.replace("|", "\\|").replace("\n", " ")


def render_summary(review: dict, placements: list[Placement], repeats: list[dict], context: dict, run_url: str) -> str:
    head = context.get("head", "")[:7]
    counts = {s: 0 for s in SEVERITIES}
    for placement in placements:
        counts[placement.finding["severity"]] += 1

    lines = [SUMMARY_MARKER, "## AI review", ""]
    lines += [review["summary"] or "No summary.", ""]

    if placements:
        lines += ["| Severity | Finding | Where | Thread |", "| --- | --- | --- | --- |"]
        for placement in placements:
            finding = placement.finding
            where = f"`{finding['path']}:{finding['line']}`" if finding["line"] else f"`{finding['path']}`"
            thread = {"line": "inline", "file": "file comment", "summary": "this comment only"}[placement.kind]
            lines.append(f"| {SEVERITY_LABEL[finding['severity']]} | {_escape_cell(finding['title'])} | {where} | {thread} |")
        lines.append("")
        unthreaded = [p for p in placements if p.kind == "summary"]
        for placement in unthreaded:
            finding = placement.finding
            lines += [f"<details><summary>{SEVERITY_LABEL[finding['severity']]}: {_escape_cell(finding['title'])}</summary>", "", finding["body"], "", "</details>", ""]
    else:
        lines += ["No new findings.", ""]

    if repeats:
        lines += [f"{len(repeats)} finding(s) matched open threads on this pull request and were not posted again.", ""]

    stats = review["stats"]
    lines.append(
        f"**{counts['important']} important · {counts['pre-existing']} pre-existing · {counts['nit']} nit.** "
        f"Hunters: {stats['hunters_run']} · candidates: {stats['candidates']} · "
        f"dropped by vote: {stats['dropped_by_vote']} · repeats dropped by the reviewer: {stats['dropped_as_repeat']}."
    )
    lines.append("")
    lines.append(f"Reviewed `{head}` with the reviewer from {context.get('reviewer', 'main')}. [Run]({run_url})")
    if context.get("bootstrap"):
        lines.append("")
        lines.append("> The reviewer is not on `main` yet, so this pull request was reviewed with its own reviewer files. After it merges, reviews always use the reviewer from `main`.")
    lines.append("")
    lines.append("Answer every inline finding before merging: fix it, or reply with the reason and resolve the conversation. Pre-existing findings need no answer here.")
    return "\n".join(lines) + "\n"


# --------------------------------------------------------------------------
# Reviewer evals
# --------------------------------------------------------------------------

LINE_TOLERANCE = 5
_CASE_ID = re.compile(r"^[a-z0-9][a-z0-9-]{0,63}$")
_SHA = re.compile(r"^[0-9a-f]{40}$")


def validate_case(case: object, case_id: str) -> dict:
    _require(isinstance(case, dict), f"{case_id}: case must be an object")
    _require(case.get("id") == case_id and _CASE_ID.match(case_id) is not None, f"{case_id}: id must match the file name and use a-z, 0-9 and -")
    for key in ("base", "change", "fix"):
        _require(isinstance(case.get(key), str) and _SHA.match(case[key]) is not None, f"{case_id}: {key} must be a full 40-character commit SHA")
    _require(isinstance(case.get("title"), str) and case["title"].strip(), f"{case_id}: title is required")
    expected = case.get("expected")
    _require(isinstance(expected, list) and expected, f"{case_id}: expected must list at least one location")
    for index, item in enumerate(expected):
        _require(isinstance(item, dict), f"{case_id}: expected[{index}] must be an object")
        _require(isinstance(item.get("path"), str) and item["path"], f"{case_id}: expected[{index}].path is required")
        start, end = item.get("start_line"), item.get("end_line")
        _require(isinstance(start, int) and isinstance(end, int) and 1 <= start <= end, f"{case_id}: expected[{index}] needs 1 <= start_line <= end_line")
    return case


def score_case(case: dict, review: dict | None) -> dict:
    """A case is caught when an Important finding lands on an expected location."""
    result = {"id": case["id"], "title": case["title"], "status": "missed", "findings": 0}
    if review is None:
        result["status"] = "error"
        return result
    result["findings"] = len(review["findings"])
    lower = False
    for finding in review["findings"]:
        for expected in case["expected"]:
            if finding["path"] != expected["path"] or finding["line"] == 0:
                continue
            if expected["start_line"] - LINE_TOLERANCE <= finding["line"] <= expected["end_line"] + LINE_TOLERANCE:
                if finding["severity"] == "important":
                    result["status"] = "caught"
                    return result
                lower = True
    if lower:
        result["status"] = "lower-severity"
    return result


def score_run(results: list[dict], baseline: dict) -> tuple[bool, str]:
    caught = sum(1 for r in results if r["status"] == "caught")
    errors = [r for r in results if r["status"] == "error"]
    floor = baseline.get("caught")
    lines = ["## Reviewer evals", ""]
    lines += ["| Case | Result | Findings |", "| --- | --- | --- |"]
    for r in sorted(results, key=lambda r: r["id"]):
        lines.append(f"| `{r['id']}`: {_escape_cell(r['title'])} | {r['status']} | {r['findings']} |")
    lines.append("")
    lines.append(f"Caught {caught} of {len(results)}.")
    passed = True
    if errors:
        passed = False
        lines.append(f"{len(errors)} case(s) produced no review output, so this run cannot be scored.")
    if floor is None:
        lines.append("No baseline is recorded yet. Record this score in `.github/review/evals/baseline.json` once you accept it.")
    elif caught < floor:
        passed = False
        lines.append(f"Lower than the baseline of {floor}: keep the current reviewer, or rerun if this looks like noise.")
    elif caught > floor:
        lines.append(f"Higher than the baseline of {floor}. Raise `caught` in `.github/review/evals/baseline.json` to {caught} in this pull request.")
    else:
        lines.append(f"Equal to the baseline of {floor}.")
    return passed, "\n".join(lines) + "\n"
