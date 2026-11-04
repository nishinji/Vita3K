#!/usr/bin/env bash
# Runs the render tests of a VitaAutotests checkout, built by build-autotests.sh, headless on a Linux
# build of Vita3K with Vulkan on lavapipe, and compares their captures with the images a Vita gave,
# with the checkout's run_tests.py. The captures and the images of the pixels that differ go to
# build/captures/ in the checkout. The tests that differ don't fail it, it only reports them.
# Usage: run-render-autotests.sh <Vita3K binary> <VitaAutotests checkout>
set -euo pipefail

VITA3K="$(realpath "$1")"
AUTOTESTS_DIR="$(realpath "$2")"
JOBS=4
TITLE_ID=VTST00000

# Only lavapipe, so a runner with a GPU gives the same results
lavapipe_icd="$(ls /usr/share/vulkan/icd.d/lvp_icd*.json | head -n 1)"
export VK_DRIVER_FILES="$lavapipe_icd" VK_ICD_FILENAMES="$lavapipe_icd"
export SDL_VIDEO_DRIVER=offscreen SDL_AUDIO_DRIVER=dummy
unset DISPLAY WAYLAND_DISPLAY

# An emulated Vita of its own for each instance, with the vpk installed
work_dir="$(mktemp -d)"
vpk="$AUTOTESTS_DIR/build/tests/vita-autotests.vpk"
for worker in $(seq 0 $((JOBS - 1))); do
    root="$work_dir/$worker"
    mkdir -p "$root/config/Vita3K" "$AUTOTESTS_DIR/build/logs"
    # Surface sync, off by default, gets what the GPU drew into the framebuffer the test reads, and
    # without draws skipped while their pipelines compile, a capture is the same in every run
    printf 'disable-surface-sync: false\nasync-pipeline-compilation: false\n' >"$root/config/Vita3K/config.yml"
    XDG_CONFIG_HOME="$root/config" XDG_DATA_HOME="$root/data" XDG_CACHE_HOME="$root/cache" \
        "$VITA3K" -B Vulkan --frontend sdl --launch-uri "psgm:play?titleid=$TITLE_ID&arg=list" "$vpk" \
        >"$AUTOTESTS_DIR/build/logs/install-$worker.log" 2>&1
done
# What decides whether the captures can hold anything: the device, and how guest memory is mapped
device_lines="$(grep -h -i -E "vulkan device|memory mapping|external host" "$AUTOTESTS_DIR/build/logs/install-0.log" || true)"
echo "$device_lines"

command="env XDG_CONFIG_HOME=$work_dir/{worker}/config XDG_DATA_HOME=$work_dir/{worker}/data"
command+=" XDG_CACHE_HOME=$work_dir/{worker}/cache $VITA3K -B Vulkan --frontend sdl -r $TITLE_ID"
command+=" --launch-uri psgm:play?titleid=$TITLE_ID&arg={test}"

cd "$AUTOTESTS_DIR"
status=0
python3 run_tests.py -v -j "$JOBS" -t 120 -E "$command" --ux0 "$work_dir/{worker}/data/Vita3K/Vita3K/ux0" tests/render \
    | tee build/render.log || status=$?

# Annotations, which the run's page shows without signing in, unlike the log and the summary
if [ -n "${GITHUB_ACTIONS:-}" ]; then
    while IFS= read -r line; do
        [ -n "$line" ] && echo "::notice title=Vita3K on lavapipe::${line//$''/}"
    done <<<"$device_lines"
    grep -E '^(PASS|FAIL|SKIP) |passed' build/render.log | while IFS= read -r line; do
        echo "::notice title=Render test::$line"
    done
fi

if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
    {
        echo "### Render tests on lavapipe"
        echo '```'
        grep -v '^      ' build/render.log
        echo '```'
        echo "The captures and the images of the pixels that differ are in the render-autotests artifact."
    } >>"$GITHUB_STEP_SUMMARY"
fi
echo "run_tests.py exited with $status"
