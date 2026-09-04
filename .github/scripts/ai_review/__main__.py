"""Entry point: read the pull request, build the context, ask Gemini, post the review."""

from __future__ import annotations

import hashlib
import os
import sys
import time

from . import context, diff, gemini
from .github import GitHub, GitHubError
from .index import RepoIndex

STICKY_MARKER = "<!-- vita3k-ai-review -->"
FINDING_MARKER = "<!-- vita3k-ai-review:finding"

SEVERITY_RANK = {"critical": 0, "major": 1, "minor": 2, "nit": 3}
SEVERITY_ICON = {"critical": "🔴", "major": "🟠", "minor": "🟡", "nit": "⚪"}
VERDICT_TEXT = {
    "approve": "No blocking issue found",
    "comment": "Comments only",
    "request_changes": "Changes requested",
}
HUMAN_REVIEW_TEXT = {
    "recommended": "⚠️ **Human review recommended**",
    "optional": "👀 **Human review optional**",
    "unnecessary": "✅ **Human review likely unnecessary**",
}

SKIP_PREFIXES = ("external/", "vcpkg/", "build/", "dist/", "data/", "docs/")
SKIP_SUFFIXES = (".inc", ".lock", ".png", ".jpg", ".ico", ".ttf", ".bin", ".zip")
MAX_INLINE_COMMENTS = 25
DEFAULT_FALLBACKS = "gemini-3.7-flash,gemini-3.5-flash,gemini-3.5-flash-lite"


def env(name: str, default: str = "") -> str:
    return os.environ.get(name, default).strip()


def env_int(name: str, default: int) -> int:
    try:
        return int(env(name) or default)
    except ValueError:
        return default


def model_chain() -> list[str]:
    """Preferred model first, then the fallbacks used when it is overloaded."""
    chain = [env("GEMINI_MODEL", "gemini-3.8-flash")]
    chain += [m.strip() for m in env("GEMINI_FALLBACK_MODELS", DEFAULT_FALLBACKS).split(",")]
    return list(dict.fromkeys(m for m in chain if m))


def reviewable(file_diff: diff.FileDiff) -> bool:
    path = file_diff.path
    return not (
        file_diff.binary
        or path.startswith(SKIP_PREFIXES)
        or path.endswith(SKIP_SUFFIXES)
        or "/nids/" in path
    )


def finding_id(finding: dict) -> str:
    raw = f"{finding['file']}|{finding['line']}|{finding['title']}"
    return hashlib.sha1(raw.encode("utf-8")).hexdigest()[:12]


def normalize(path: str) -> str:
    path = path.replace("\\", "/")
    for prefix in ("./", "a/", "b/"):
        if path.startswith(prefix):
            path = path[len(prefix):]
    return path.lstrip("/")


def validate(findings: list[dict], files: list[diff.FileDiff]) -> tuple[list[dict], list[dict]]:
    """Split findings into ones that can be anchored inline and ones that can only go in the summary."""
    anchors = {f.path: f.right_lines() for f in files}
    inline: list[dict] = []
    summary_only: list[dict] = []

    for finding in findings:
        path = normalize(str(finding.get("file") or ""))
        if not path:
            continue
        if path not in anchors:
            match = [p for p in anchors if p.endswith("/" + path) or os.path.basename(p) == path]
            if len(match) == 1:
                path = match[0]
        finding["file"] = path

        try:
            line = int(finding.get("line") or 0)
            end_line = int(finding.get("end_line") or line)
        except (TypeError, ValueError):
            continue
        if line < 1:
            continue
        finding["line"] = line
        finding["end_line"] = max(line, end_line)
        if finding.get("severity") not in SEVERITY_RANK:
            finding["severity"] = "minor"
        if finding.get("confidence") not in ("high", "medium", "low"):
            finding["confidence"] = "medium"

        # A finding in a file the diff does not touch is still worth reporting, just not inline.
        if anchors.get(path, set()).issuperset({line, finding["end_line"]}):
            inline.append(finding)
        else:
            summary_only.append(finding)

    def order(finding: dict) -> tuple:
        return SEVERITY_RANK.get(finding["severity"], 4), finding["file"], finding["line"]

    return sorted(inline, key=order), sorted(summary_only, key=order)


def comment_body(finding: dict) -> str:
    icon = SEVERITY_ICON.get(finding["severity"], "🟡")
    parts = [
        f"{FINDING_MARKER}:{finding_id(finding)} -->",
        f"**{icon} {finding['severity']} · {finding['title']}**",
        "",
        finding.get("detail", "").strip(),
    ]
    suggestion = (finding.get("suggestion") or "").strip("\n")
    if suggestion and "```" not in suggestion:
        parts += ["", "```suggestion", suggestion, "```"]
    if finding.get("confidence") == "low":
        parts += ["", "<sub>Low confidence: verify before acting on this.</sub>"]
    return "\n".join(parts)


def review_comment(finding: dict) -> dict:
    payload = {"path": finding["file"], "side": "RIGHT", "line": finding["end_line"], "body": comment_body(finding)}
    if finding["end_line"] > finding["line"]:
        payload["start_line"] = finding["line"]
        payload["start_side"] = "RIGHT"
    return payload


def human_review_line(result: dict, findings: list[dict]) -> str:
    """Whether a person should still read this, per the model unless a serious finding overrules it."""
    call = result.get("human_review")
    reason = (result.get("human_review_reason") or "").strip().rstrip(".")
    if call not in HUMAN_REVIEW_TEXT:
        call = "optional"
    serious = any(
        f["severity"] == "critical" or (f["severity"] == "major" and f["confidence"] == "high")
        for f in findings
    )
    if serious and call != "recommended":
        call, reason = "recommended", "a high-confidence major finding needs a human decision"
    return HUMAN_REVIEW_TEXT[call] + (f" — {reason}." if reason else ".")


def unavailable_body(reason: str, sha: str) -> str:
    quota = "quota" in reason.lower() or "429" in reason
    return "\n".join([
        STICKY_MARKER,
        "## 🤖 Gemini review",
        "",
        "**Not reviewed.** Every model was unavailable"
        + (" and the free-tier quota is spent." if quota else ", the API was busy."),
        "",
        "Comment `/ai-review` to try again.",
        "",
        "<details><summary>Last error</summary>",
        "",
        "```",
        reason[:1200],
        "```",
        "",
        "</details>",
        "",
        f"<sub>commit `{sha[:7]}`</sub>",
    ])


def blob_link(repo: str, sha: str, finding: dict) -> str:
    return f"https://github.com/{repo}/blob/{sha}/{finding['file']}#L{finding['line']}"


def sticky_body(
    repo: str,
    sha: str,
    result: dict,
    inline: list[dict],
    summary_only: list[dict],
    posted_inline: bool,
    notes: list[str],
) -> str:
    verdict = VERDICT_TEXT.get(result.get("verdict", "comment"), "Comments only")
    lines = [
        STICKY_MARKER,
        "## 🤖 Gemini review",
        "",
        f"**{verdict}** · {len(inline) + len(summary_only)} finding(s)",
        "",
        human_review_line(result, inline + summary_only),
        "",
        (result.get("summary") or "").strip(),
    ]

    if inline:
        lines += ["", "### Inline findings" if posted_inline else "### Findings", ""]
        for finding in inline:
            icon = SEVERITY_ICON.get(finding["severity"], "🟡")
            link = blob_link(repo, sha, finding)
            lines.append(f"- {icon} **{finding['severity']}** [`{finding['file']}:{finding['line']}`]({link}) — {finding['title']}")
            if not posted_inline:
                lines.append(f"  {finding.get('detail', '').strip()}")

    if summary_only:
        lines += [
            "",
            "<details><summary>Findings outside the diff (%d)</summary>" % len(summary_only),
            "",
        ]
        for finding in summary_only:
            icon = SEVERITY_ICON.get(finding["severity"], "🟡")
            lines.append(f"- {icon} **{finding['severity']}** `{finding['file']}:{finding['line']}` — {finding['title']}")
            lines.append(f"  {finding.get('detail', '').strip()}")
        lines += ["", "</details>"]

    if notes:
        lines += ["", *[f"> [!NOTE]\n> {note}" for note in notes]]

    usage = result.get("_usage", {})
    lines += [
        "",
        "<sub>{model} ({thinking}) · {tokens} prompt tokens · commit `{sha}`. Automated review; it can be wrong.</sub>".format(
            model=result.get("_model", "gemini"),
            thinking=(result.get("_thinking_level") or "no") + " thinking",
            tokens=usage.get("promptTokenCount", "?"),
            sha=sha[:7],
        ),
    ]
    return "\n".join(lines)


def main() -> int:
    # Leave the job enough room to still post whatever the model managed to produce.
    deadline = time.monotonic() + env_int("AI_REVIEW_DEADLINE_SECONDS", 22 * 60)
    token = env("GITHUB_TOKEN")
    api_key = env("GEMINI_API_KEY")
    repo = env("GITHUB_REPOSITORY")
    number = env_int("PR_NUMBER", 0)
    root = env("REPO_ROOT", ".")
    dry_run = env("AI_REVIEW_DRY_RUN") == "1"

    if not (token and repo and number):
        print("::error::GITHUB_TOKEN, GITHUB_REPOSITORY and PR_NUMBER are required")
        return 1
    if not api_key and not dry_run:
        print("::warning::GEMINI_API_KEY is not set; skipping the AI review")
        return 0

    gh = GitHub(token, repo)
    pull = gh.pull(number)
    head_sha = (pull.get("head") or {}).get("sha", "")

    files = [f for f in diff.parse(gh.pull_diff(number)) if reviewable(f)]
    max_files = env_int("AI_REVIEW_MAX_FILES", 60)
    notes: list[str] = []
    if len(files) > max_files:
        files = sorted(files, key=lambda f: -sum(len(h.lines) for h in f.hunks))[:max_files]
        notes.append(f"Only the {max_files} largest changed files were reviewed.")
    if not files:
        print("nothing reviewable in this pull request")
        return 0

    print(f"reviewing {len(files)} file(s) at {head_sha[:7]}")
    index = RepoIndex(root)
    prompt = context.build(pull, files, index, env_int("AI_REVIEW_MAX_CONTEXT_CHARS", 400_000))
    print(f"prompt: {len(prompt)} chars from {len(index.text)} indexed files")

    dump = env("AI_REVIEW_DUMP_PROMPT")
    if dump:
        with open(dump, "w", encoding="utf-8") as handle:
            handle.write(prompt)

    if dry_run:
        print(prompt[:2000])
        return 0

    try:
        result = gemini.generate(
            api_key=api_key,
            models=model_chain(),
            user_prompt=prompt,
            thinking_level=env("GEMINI_THINKING_LEVEL", "high"),
            max_output_tokens=env_int("GEMINI_MAX_OUTPUT_TOKENS", 32768),
            deadline=deadline,
        )
    except gemini.GeminiUnavailable as outage:
        # Say so on the pull request; a warning buried in the job log reaches nobody.
        gh.upsert_sticky_comment(number, STICKY_MARKER, unavailable_body(str(outage), head_sha))
        print(f"::warning::AI review skipped, the Gemini API was unavailable: {outage}")
        return 0
    if result.get("_finish_reason") == "MAX_TOKENS":
        notes.append("The model hit its output limit, so the review may be incomplete.")

    inline, summary_only = validate(result.get("findings") or [], files)
    print(f"findings: {len(inline)} inline, {len(summary_only)} summary-only")

    # This review supersedes the last one, so collapse its comments instead of leaving them to
    # be re-read. Collapsing keeps the thread and any human replies; deleting would not.
    collapsed = 0
    try:
        stale = gh.live_review_comments(number, FINDING_MARKER)
    except GitHubError as error:
        print(f"::warning::could not list previous review comments: {error}")
        stale = []
    for node_id in stale:
        try:
            gh.minimize_comment(node_id)
            collapsed += 1
        except GitHubError as error:
            print(f"::warning::could not collapse a previous comment: {error}")
    if collapsed:
        print(f"collapsed {collapsed} comment(s) from the previous review")

    fresh = inline[:MAX_INLINE_COMMENTS]
    posted_inline = False
    if fresh:
        try:
            gh.create_review(
                number,
                head_sha,
                f"Gemini review: {len(fresh)} inline finding(s). See the summary comment for the rest.\n\n"
                + human_review_line(result, inline + summary_only),
                [review_comment(f) for f in fresh],
            )
            posted_inline = True
        except GitHubError as error:
            print(f"::warning::inline review rejected, falling back to the summary comment: {error}")
            notes.append("Inline comments could not be placed, so the findings are listed above instead.")
    elif inline:
        posted_inline = True

    gh.upsert_sticky_comment(
        number, STICKY_MARKER, sticky_body(repo, head_sha, result, inline, summary_only, posted_inline, notes)
    )
    print("review posted")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except gemini.GeminiUnavailable as outage:
        # Google being busy is not a defect in the pull request, so do not fail its checks.
        print(f"::warning::AI review skipped, the Gemini API was unavailable: {outage}")
        sys.exit(0)
    except (gemini.GeminiError, GitHubError) as failure:
        print(f"::error::{failure}")
        sys.exit(1)
