#!/usr/bin/env bash
set -euo pipefail

CLANG_FORMAT_BIN="${CLANG_FORMAT_BIN:-clang-format}"
PATCH_FILE="${PATCH_FILE:-clang-format.patch}"
MAX_ANNOTATIONS="${MAX_ANNOTATIONS:-10}"
MAX_SUMMARY_BYTES="${MAX_SUMMARY_BYTES:-900000}"

if [[ $# -eq 0 ]]; then
    set -- vita3k tools
fi

"$CLANG_FORMAT_BIN" --version

mapfile -t files < <(git ls-files -- "$@" | grep -E '\.((c|C)(c|pp|xx|\+\+)?|(h|H)h?(pp|xx|\+\+)?|ino|pde|proto|cu)$' || true)
if [[ ${#files[@]} -eq 0 ]]; then
    echo "No source files to check."
    exit 0
fi

work_dir="$(mktemp -d)"
trap 'rm -rf "$work_dir"' EXIT

export CLANG_FORMAT_BIN work_dir
printf '%s\0' "${files[@]}" | xargs -0 -n 16 -P "$(nproc 2>/dev/null || echo 4)" bash -c '
    for file; do
        out="$work_dir/$(printf "%s" "$file" | tr "/" "%").patch"
        "$CLANG_FORMAT_BIN" --style=file "$file" >"$out.fmt" || { echo "clang-format failed on $file" >&2; exit 255; }
        diff -u --label "a/$file" --label "b/$file" "$file" "$out.fmt" >"$out" || [[ $? -eq 1 ]] || exit 255
        rm -f "$out.fmt"
        [[ -s "$out" ]] || rm -f "$out"
    done
' _

: >"$PATCH_FILE"
for file in "${files[@]}"; do
    out="$work_dir/$(printf '%s' "$file" | tr '/' '%').patch"
    [[ -f "$out" ]] && cat "$out" >>"$PATCH_FILE"
done

if [[ ! -s "$PATCH_FILE" ]]; then
    rm -f "$PATCH_FILE"
    echo "All ${#files[@]} files are formatted correctly."
    exit 0
fi

mapfile -t failing < <(sed -n 's|^+++ b/||p' "$PATCH_FILE")

esc=$'\e'
sed -E \
    -e "s/^(---|\+\+\+) .*/${esc}[1m&${esc}[0m/" \
    -e "t" \
    -e "s/^@@.*/${esc}[36m&${esc}[0m/" \
    -e "s/^-.*/${esc}[31m&${esc}[0m/" \
    -e "s/^\+.*/${esc}[32m&${esc}[0m/" \
    "$PATCH_FILE"

echo
echo "${#failing[@]} file(s) are not formatted correctly:"
printf '  %s\n' "${failing[@]}"
echo "Run ./format.sh or apply the patch with: git apply ${PATCH_FILE##*/}"

if [[ -n "${GITHUB_ACTIONS:-}" ]]; then
    awk -v max="$MAX_ANNOTATIONS" '
        function esc_data(s) { gsub(/%/, "%25", s); gsub(/\r/, "%0D", s); gsub(/\n/, "%0A", s); return s }
        function esc_prop(s) { s = esc_data(s); gsub(/:/, "%3A", s); gsub(/,/, "%2C", s); return s }
        function flush() {
            if (body == "" || count >= max) return
            count++
            printf "::error file=%s,line=%d,endLine=%d,title=%s::%s\n", esc_prop(file), first, last,
                esc_prop("clang-format"), esc_data("Formatting differs from clang-format:\n" body)
        }
        /^\+\+\+ b\// { flush(); body = ""; file = substr($0, 7); next }
        /^--- / { next }
        /^@@/ {
            flush()
            split($2, r, ","); line = substr(r[1], 2) + 0; body = $0 "\n"; first = 0; last = 0
            next
        }
        /^\\/ { body = body $0 "\n"; next }
        {
            body = body $0 "\n"
            c = substr($0, 1, 1)
            if (c == "+") { if (!first) first = line; if (last < first) last = first; next }
            if (c == "-") { if (!first) first = line; last = line }
            line++
        }
        END { flush() }
    ' "$PATCH_FILE"
fi

if [[ -n "${GITHUB_STEP_SUMMARY:-}" ]]; then
    {
        echo "## clang-format check failed"
        echo
        echo "${#failing[@]} file(s) need formatting. Run \`./format.sh\` or download the patch artifact of this run and use \`git apply ${PATCH_FILE##*/}\`."
        echo
        if [[ $(wc -c <"$PATCH_FILE") -le $MAX_SUMMARY_BYTES ]]; then
            echo '```diff'
            cat "$PATCH_FILE"
            echo '```'
        else
            echo "The diff is too large to show here; see the job log or the artifact."
            echo
            printf -- '- `%s`\n' "${failing[@]}"
        fi
    } >>"$GITHUB_STEP_SUMMARY"
fi

exit 1
