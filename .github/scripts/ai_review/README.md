# AI review

Reviews pull requests with Gemini. Driven by [`.github/workflows/ai-review.yml`](../../workflows/ai-review.yml),
which runs when a pull request is opened, reopened, marked ready for review, or pushed to.

## Asking for another review

Comment `/ai-review` on the pull request. It reviews the current head, can be repeated as often as
you like, and reacts 👀 to your comment so you know it started — the run is not a check on the pull
request, so that reaction is the only immediate feedback. Only owners, organisation members and
collaborators can trigger it; otherwise anyone passing by could spend the API quota.

Re-running the job from the Actions tab also works and needs no comment, but it replays the commit
that run was for, which makes it the right tool for retrying a failed run rather than for reviewing
new work. `workflow_dispatch` takes a pull request number for the same job.

## What the model is given

A diff alone is not enough to catch the defects that matter here, so the prompt also carries:

- the full text of every changed file after the change, with line numbers;
- the counterpart header or translation unit of each changed file;
- the definition of every symbol the diff calls into, looked up across the tree;
- existing callers of every symbol the diff defines or whose declaration it changes,
  which is what catches a signature change that a caller elsewhere still uses the old way;
- the list of files that include a changed header.

Sections are filled in that priority order until the budget runs out. `external/`, generated NID
tables and binary files are never indexed or reviewed.

## Output

- One sticky summary comment, rewritten on every push rather than piling up.
- A call on whether a person should still read the change — `recommended`, `optional` or
  `unnecessary` with a one-line reason — shown on both the review and the summary comment. The
  model decides, except that a critical finding, or a high-confidence major one, always forces
  `recommended`.
- Inline review comments, with `suggestion` blocks when the model can give exact replacement source.
  Findings anchored outside the diff are listed in the summary instead.

Every push supersedes the previous review, so its inline comments are collapsed as **Outdated**
rather than deleted: the threads and any human replies survive, but nobody has to read stale
findings. GitHub has no API for collapsing the review header itself, so those short lines remain.

## Security

The workflow uses `pull_request_target` because a `pull_request` run from a fork cannot read
`GEMINI_API_KEY`. That trigger runs with a writable token, so the pull request tree is checked out
into `pr/` with `persist-credentials: false` and is only ever *read* — nothing in it is built,
installed or executed, and the reviewer script itself comes from the base branch. Keep it that way:
do not add a build, a dependency install, or any step that runs a file from `pr/`.
