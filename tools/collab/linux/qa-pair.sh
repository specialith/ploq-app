#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
#
# Linux version of tools/collab/qa-auto/run-pair.ps1 (local relay only): the relay plus two
# offscreen copies of a development build, "Alice" and "Bob", run a scenario's autobot
# scripts (COLLAB_AUTOBOT, development builds only) on the same score, save, and
# compare.py diffs the two saved scores. Look for "SYNC: IDENTICAL".
# Nothing ever opens a window: Qt's offscreen platform, DISPLAY/WAYLAND_DISPLAY unset
# (under WSL, WSLg would otherwise show the apps on the Windows desktop), no sound server.
#
#   tools/collab/linux/qa-pair.sh <scenario> [<ploq binary>]
#     scenario: a name from tools/collab/qa-auto/scenarios (basic, burst, collide, undo, ...)
#     binary:   default build.dev.install/bin/ploq
#   Env: RELAY_JS (default ../ploq-server/relay/server.mjs), NODE (default node), RUNS (default builds/qa-runs-linux)
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
Q="${ROOT}/tools/collab/qa-auto"
NAME="${1:?usage: qa-pair.sh <scenario> [<ploq binary>]}"
EXE="${2:-${ROOT}/build.dev.install/bin/ploq}"
RELAY_JS="${RELAY_JS:-${ROOT}/../ploq-server/relay/server.mjs}"
NODE="${NODE:-node}"
RUNS="${RUNS:-${ROOT}/builds/qa-runs-linux}"
SCORE="${SCORE:-${ROOT}/demos/Amazing_grace.mscz}"
TIMEOUT=${TIMEOUT:-180}

unset DISPLAY WAYLAND_DISPLAY PULSE_SERVER
export QT_QPA_PLATFORM=offscreen MU_QT_QPA_PLATFORM=offscreen
export PLOQ_UPDATE_URL=http://127.0.0.1:9/no-updates-in-qa

RUN="${RUNS}/${NAME}"
rm -rf "${RUN}" && mkdir -p "${RUN}"
LOG="${RUN}/harness.log"
say() { echo "$(date +%H:%M:%S.%3N) $*" | tee -a "${LOG}"; }

CONFIG="${XDG_CONFIG_HOME:-$HOME/.config}/Ploq"
DATA="${XDG_DATA_HOME:-$HOME/.local/share}/Ploq/PloqDevelopment"
# No note playback while the bots edit (as run-pair.ps1 does), only if the settings have no [score] yet.
INI="${CONFIG}/PloqDevelopment.ini"
MUTED=false
if ! grep -q '^\[score\]' "${INI}" 2>/dev/null; then
    mkdir -p "${CONFIG}"
    printf '\n[score]\nnote\\playOnClick=false\nchord\\playOnAddNote=false\nharmony\\play\\onedit=false\n' >> "${INI}"
    MUTED=true
fi

nice -n 10 "${EXE}" -o "${RUN}/base.mscz" "${SCORE}" > "${RUN}/convert.log" 2>&1
[[ -f "${RUN}/base.mscz" ]] || { say "!!! couldn't convert ${SCORE}"; exit 1; }
cp "${RUN}/base.mscz" "${RUN}/Alice.mscz"; cp "${RUN}/base.mscz" "${RUN}/Bob.mscz"
rm -f "${DATA}/session/session.json"

PORT=$(( 40000 + RANDOM % 9000 ))
ROOM="qa-${NAME}-${RANDOM}"
"${NODE}" "${RELAY_JS}" "${PORT}" > "${RUN}/relay.log" 2> "${RUN}/relay.err" &
RELAY=$!
sleep 0.5

declare -A PID
start_app() {
    local who=$1 color=$2
    { echo "var RUN = '${RUN}'; var ME = '${who}';"; cat "${Q}/common.js"; echo; cat "${Q}/scenarios/${NAME}_${who:0:1}.js" 2>/dev/null || cat "${Q}/scenarios/idle.js"; } > "${RUN}/${who}.js"
    COLLAB_NAME=${who} COLLAB_COLOR=${color} COLLAB_AUTOBOT="${RUN}/${who}.js" \
    COLLAB_SERVER="ws://127.0.0.1:${PORT}" COLLAB_ROOM="${ROOM}" \
        nice -n 10 "${EXE}" "${RUN}/${who}.mscz" > "${RUN}/stdout-${who}.log" 2>&1 &
    PID[$who]=$!
    say "${who} pid ${PID[$who]}"
}
start_app Alice "#e6194b"
start_app Bob "#f58231"

phase=start; result="?"; go0=false; last_len=-1; quiet_since=$(date +%s)
deadline=$(( $(date +%s) + TIMEOUT ))
while [[ "${result}" == "?" ]]; do
    for who in Alice Bob; do
        kill -0 "${PID[$who]}" 2>/dev/null || { say "!!! ${who} EXITED during ${phase}"; result="CRASH:${who}"; }
    done
    [[ "${result}" == "?" ]] || break
    (( $(date +%s) > deadline )) && { say "!!! TIMEOUT in phase ${phase}"; result="TIMEOUT:${phase}"; break; }
    if ! $go0 && [[ -f "${RUN}/Alice.started" && -f "${RUN}/Bob.started" ]]; then
        go0=true; say "both in the room -> go"; echo go > "${RUN}/go0"
    fi
    len=$(stat -c %s "${RUN}/relay.log")
    if [[ "${len}" != "${last_len}" ]]; then last_len=${len}; quiet_since=$(date +%s); fi
    if [[ "${phase}" == start && -f "${RUN}/Alice.done" && -f "${RUN}/Bob.done" ]] && (( $(date +%s) - quiet_since >= 4 )); then
        say "both done, quiet 4s -> save"; echo go > "${RUN}/save.flag"; phase=saving
    fi
    if [[ "${phase}" == saving && -f "${RUN}/Alice.saved" && -f "${RUN}/Bob.saved" ]]; then
        say "both saved"; result=OK; break
    fi
    sleep 0.1
done
sleep 0.5
for who in Alice Bob; do kill "${PID[$who]}" 2>/dev/null; done
kill "${RELAY}" 2>/dev/null
sleep 1
for who in Alice Bob; do kill -9 "${PID[$who]}" 2>/dev/null; done
rm -f "${DATA}/session/session.json"
if $MUTED; then
    python3 - "${INI}" <<'EOF'
import sys
p = sys.argv[1]
s = open(p).read()
open(p, "w").write(s.replace("\n[score]\nnote\\playOnClick=false\nchord\\playOnAddNote=false\nharmony\\play\\onedit=false\n", ""))
EOF
fi
# The app logs (one per process) that mention this run's scores.
for who in Alice Bob; do
    f=$(grep -l -F "${RUN}/${who}.mscz" $(ls -t "${DATA}/logs/"*.log 2>/dev/null) 2>/dev/null | head -n1)
    if [[ -n "${f}" ]]; then cp "${f}" "${RUN}/app-${who}.log"; else say "(no log found for ${who})"; fi
done
say "RESULT ${result}"
python3 -I "${Q}/compare.py" "${RUN}"
