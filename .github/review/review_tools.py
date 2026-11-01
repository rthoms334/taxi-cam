"""Pure helpers for the pull-request review pipeline.

Everything here is deterministic and offline: risk classification, diff
parsing, findings validation and comment rendering. Network
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
    ("high", "src/app/launcher.*", "writes into the simulator process and starts the bridge there"),
    ("high", "src/app/json_value.hpp", "parses the installation record Setup acts on"),
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
    ("medium", "*/AGENTS.md", "rules for coding agents"),
    ("medium", "CLAUDE.md", "rules for coding agents"),
    ("medium", "*/CLAUDE.md", "rules for coding agents"),
    ("medium", "CLAUDE.local.md", "rules for coding agents"),
    ("medium", ".claude/*", "settings and instructions for coding agents"),
    ("medium", ".mcp.json", "tools for coding agents"),
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


def classify(paths: list[str], expected_count: int | None = None, old_names: list[str] = ()) -> dict:
    """Classify a pull request from every path it touches.

    `paths` are the files GitHub lists for the pull request and `old_names`
    the previous names of renamed ones. `expected_count` is GitHub's
    changed-file count. The files endpoint stops listing at 3000 files, so
    fewer listed files than that count means the classifier cannot see every
    path and the change is treated as high risk. Old names are classified but
    never counted, or renames could hide a truncated list.
    """
    listed = {p.strip() for p in paths if p.strip()}
    unique = sorted(listed | {p.strip() for p in old_names if p.strip()})
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
    if expected_count is not None and len(listed) < expected_count:
        tier = "high"
        notes.append(f"GitHub reports {expected_count} changed files but listed {len(listed)}; the file list is incomplete.")
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


def commentable_lines(diff: str, context: bool = True) -> dict[str, set[int]]:
    """Map each path in the diff to the head-side lines GitHub accepts.

    GitHub accepts review comments on added and context lines inside a hunk
    (side RIGHT). Files changed without hunks (binary, mode-only, pure
    renames) and deleted files map to an empty set: they accept file-level
    comments only, so an Important finding on a deletion still gets a thread.
    With `context=False` only the lines a change actually touched are
    returned: added lines, and the head lines on either side of each
    deletion, where a finding about removed code is anchored.
    """
    files: dict[str, set[int]] = {}
    header_path: str | None = None
    deleted = False
    current: str | None = None
    old_left = new_left = 0
    new_line = 0

    def close_file() -> None:
        if header_path is not None:
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
                if current is not None and not context:
                    touched = files.setdefault(current, set())
                    touched.add(new_line)
                    if new_line > 1:
                        touched.add(new_line - 1)
                old_left -= 1
            else:  # context line; an empty line is context with its space trimmed
                if current is not None:
                    lines = files.setdefault(current, set())
                    if context:
                        lines.add(new_line)
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
        elif raw.startswith("--- ") and deleted:
            header_path = _diff_path(raw[4:]) or header_path
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
_REVIEWED = re.compile(r"<!-- taxi-cam-ai-review-reviewed sha=([0-9a-f]{40}) -->")
_REVIEWED_TEXT = re.compile(r"Reviewed `([0-9a-f]{7,40})`")  # summaries written before the marker existed
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
    complete = data.get("complete")
    _require(isinstance(complete, bool), "complete must be true or false")
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
    return {"summary": summary.strip(), "findings": clean, "stats": clean_stats, "complete": complete}


def fingerprint(finding: dict) -> str:
    """Identify a finding across pushes by severity, file and title."""
    words = re.findall(r"[a-z0-9]+", finding["title"].lower())
    key = f"{finding['severity']}|{finding['path']}|{' '.join(words)}"
    return hashlib.sha1(key.encode("utf-8")).hexdigest()[:12]


BOT_LOGINS = ("github-actions[bot]", "github-actions")  # REST and GraphQL spellings


def last_reviewed_sha(comments: list[dict]) -> str | None:
    """The head commit of the last review posted on this pull request.

    `comments` are REST issue comments. Only the reviewer's own summary
    comment counts, so nobody else can move the follow-up starting point.
    """
    found = None
    for comment in comments:
        if (comment.get("user") or {}).get("login") not in BOT_LOGINS:
            continue
        body = comment.get("body") or ""
        if SUMMARY_MARKER in body:
            match = _REVIEWED.search(body) or _REVIEWED_TEXT.search(body)
            found = match.group(1) if match else found
    return found


def follow_up_scope(findings: list[dict], lines: dict[str, set[int]]) -> tuple[list[dict], list[dict]]:
    """Keep only what a follow-up review may report.

    A follow-up covers the lines touched since the last review (see
    commentable_lines with context=False), so only Important findings on
    those lines are kept. A file changed without line changes (a rename,
    binary or mode change) is in scope as a whole. Everything else is dropped before posting: re-reviewing code
    that was already reviewed would keep finding new things to change.
    """
    kept, dropped = [], []
    for finding in findings:
        in_scope = (finding["severity"] == "important" and finding["path"] in lines
                    and (finding["line"] == 0 or finding["line"] in lines[finding["path"]] or not lines[finding["path"]]))
        (kept if in_scope else dropped).append(finding)
    return kept, dropped


FULL_SCOPE = """# Scope: full review

Review the whole change in `change.diff` with the procedure in `prompt.md`.
"""

FOLLOW_UP_SCOPE = """# Scope: follow-up review

The last review on this pull request covered commit `{since}`. `followup.diff` holds only what changed since then in this pull request's files. `change.diff` is the whole pull request, for context only.

- Review only the code that `followup.diff` changes. Do not report anything about code it does not change, even if you notice it.
- Check the findings in `previous.json`: if one is still present on lines that `followup.diff` changes, report it again.
- Report only `important` findings. Report no nits and no pre-existing findings.
- Run at most two hunters: General correctness, and the one area hunter that best matches `followup.diff`. Keep at most four candidates. Voting is unchanged.

Findings outside this scope are removed before posting.
"""


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
    ("Anthropic key", re.compile(r"sk-ant-[A-Za-z0-9_-]{40,}")),
    ("GitHub token", re.compile(r"\bgh[opsu]_[A-Za-z0-9]{20,}")),
    ("GitHub token", re.compile(r"\bgithub_pat_[A-Za-z0-9_]{20,}")),
    ("token in a URL", re.compile(r"x-access-token:[A-Za-z0-9_.-]{20,}@")),
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


def previous_findings(threads: list[dict]) -> list[dict]:
    """Summarize the reviewer's earlier threads for the next review.

    `threads` are GraphQL reviewThreads nodes. Each finding keeps whether its
    thread was resolved and the replies of people with write access. Replies
    from anyone else are left out: on a public repository anyone can reply,
    and the reviewer must not read their text as context.
    """
    result = []
    for thread in threads:
        comments = ((thread.get("comments") or {}).get("nodes")) or []
        if not comments or ((comments[0].get("author") or {}).get("login")) not in BOT_LOGINS:
            continue
        body = comments[0].get("body") or ""
        if not _FP.search(body):
            continue
        replies = [{"author": (c.get("author") or {}).get("login", ""), "body": (c.get("body") or "")[:2000]}
                   for c in comments[1:] if c.get("authorAssociation") in TRUSTED_ASSOCIATIONS]
        result.append({
            "path": thread.get("path"),
            "line": thread.get("line") or thread.get("originalLine") or 0,
            "resolved": bool(thread.get("isResolved")),
            "finding": body.split("<sub>", 1)[0].strip()[:3000],
            "replies": replies,
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
    if context.get("mode") == "follow-up":
        lines.append(f"Follow-up review: only the changes since `{context.get('since', '')[:7]}` were reviewed, and only Important findings on them are reported.")
        if context.get("scope_dropped"):
            lines.append(f"{context['scope_dropped']} finding(s) outside that scope were not posted.")
        lines.append("")
    elif context.get("mode") == "skip":
        lines.append(f"No file in this pull request changed since `{context.get('since', '')[:7]}`, so nothing was reviewed again.")
        lines.append("")
    lines.append(f"Reviewed `{head}` with the reviewer from {context.get('reviewer', 'main')}. [Run]({run_url})")
    if context.get("bootstrap"):
        lines.append("")
        lines.append("> The reviewer is not on `main` yet, so this pull request was reviewed with its own reviewer files. After it merges, reviews always use the reviewer from `main`.")
    lines.append("")
    lines.append("Answer every inline finding before merging: fix it, or reply with the reason and resolve the conversation. Pre-existing findings need no answer here.")
    if len(context.get("head", "")) == 40:
        lines.append(f"<!-- taxi-cam-ai-review-reviewed sha={context['head']} -->")
    return "\n".join(lines) + "\n"

