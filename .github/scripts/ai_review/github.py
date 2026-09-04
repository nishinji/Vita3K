"""The slice of the GitHub REST API this reviewer needs, stdlib only."""

from __future__ import annotations

import json
import time
import urllib.error
import urllib.request

RETRY_STATUS = {429, 500, 502, 503, 504}

MINIMIZE_MUTATION = """
mutation($id: ID!) {
  minimizeComment(input: {subjectId: $id, classifier: OUTDATED}) {
    minimizedComment { isMinimized }
  }
}
"""

REVIEW_THREADS_QUERY = """
query($owner: String!, $name: String!, $number: Int!) {
  repository(owner: $owner, name: $name) {
    pullRequest(number: $number) {
      reviewThreads(first: 100) {
        nodes { comments(first: 20) { nodes { id body isMinimized } } }
      }
    }
  }
}
"""


class GitHubError(RuntimeError):
    def __init__(self, status: int, body: str):
        super().__init__(f"HTTP {status}: {body[:600]}")
        self.status = status
        self.body = body


class GitHub:
    def __init__(self, token: str, repo: str, api_url: str = "https://api.github.com"):
        self.token = token
        self.repo = repo
        self.api_url = api_url.rstrip("/")

    def _request(self, method: str, path: str, data: dict | None = None, accept: str = "application/vnd.github+json"):
        request = urllib.request.Request(
            f"{self.api_url}{path}",
            data=json.dumps(data).encode("utf-8") if data is not None else None,
            headers={
                "Accept": accept,
                "Authorization": f"Bearer {self.token}",
                "X-GitHub-Api-Version": "2022-11-28",
                "Content-Type": "application/json",
                "User-Agent": "vita3k-ai-review",
            },
            method=method,
        )
        for attempt in range(3):
            try:
                with urllib.request.urlopen(request, timeout=60) as response:
                    raw = response.read().decode("utf-8")
                    if accept.endswith(".diff") or accept.endswith(".patch"):
                        return raw
                    return json.loads(raw) if raw else {}
            except urllib.error.HTTPError as error:
                body = error.read().decode("utf-8", "replace")
                if error.code not in RETRY_STATUS or attempt == 2:
                    raise GitHubError(error.code, body) from error
            except (urllib.error.URLError, TimeoutError):
                if attempt == 2:
                    raise
            time.sleep(2 ** attempt * 3)
        raise GitHubError(0, "unreachable")

    # -- reads -------------------------------------------------------------

    def pull(self, number: int) -> dict:
        return self._request("GET", f"/repos/{self.repo}/pulls/{number}")

    def pull_diff(self, number: int) -> str:
        return self._request(
            "GET", f"/repos/{self.repo}/pulls/{number}", accept="application/vnd.github.v3.diff"
        )

    def issue_comments(self, number: int) -> list[dict]:
        out: list[dict] = []
        for page in range(1, 6):
            batch = self._request("GET", f"/repos/{self.repo}/issues/{number}/comments?per_page=100&page={page}")
            out.extend(batch)
            if len(batch) < 100:
                break
        return out

    # -- writes ------------------------------------------------------------

    def graphql(self, query: str, variables: dict) -> dict:
        result = self._request("POST", "/graphql", {"query": query, "variables": variables})
        if result.get("errors"):
            raise GitHubError(200, str(result["errors"]))
        return result.get("data") or {}

    def minimize_comment(self, node_id: str) -> None:
        """Collapse a comment as outdated. Only GraphQL can do this; REST has no equivalent."""
        self.graphql(MINIMIZE_MUTATION, {"id": node_id})

    def live_review_comments(self, number: int, marker: str) -> list[str]:
        """Node ids of our own review comments that are not collapsed yet."""
        owner, _, name = self.repo.partition("/")
        data = self.graphql(REVIEW_THREADS_QUERY, {"owner": owner, "name": name, "number": number})
        threads = (((data.get("repository") or {}).get("pullRequest") or {}).get("reviewThreads") or {})
        found: list[str] = []
        for thread in threads.get("nodes") or []:
            for comment in (thread.get("comments") or {}).get("nodes") or []:
                if marker in (comment.get("body") or "") and not comment.get("isMinimized"):
                    found.append(comment["id"])
        return found

    def create_issue_comment(self, number: int, body: str) -> dict:
        return self._request("POST", f"/repos/{self.repo}/issues/{number}/comments", {"body": body})

    def update_issue_comment(self, comment_id: int, body: str) -> dict:
        return self._request("PATCH", f"/repos/{self.repo}/issues/comments/{comment_id}", {"body": body})

    def create_review(self, number: int, commit_id: str, body: str, comments: list[dict]) -> dict:
        return self._request(
            "POST",
            f"/repos/{self.repo}/pulls/{number}/reviews",
            {"commit_id": commit_id, "event": "COMMENT", "body": body, "comments": comments},
        )

    def upsert_sticky_comment(self, number: int, marker: str, body: str) -> dict:
        """One comment per pull request, rewritten on every push instead of piling up."""
        for comment in self.issue_comments(number):
            if marker in (comment.get("body") or ""):
                return self.update_issue_comment(comment["id"], body)
        return self.create_issue_comment(number, body)
