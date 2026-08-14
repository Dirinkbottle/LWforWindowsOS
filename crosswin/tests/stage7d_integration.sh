#!/usr/bin/env bash
# Runs Stage 7D's nested-Weston logical clipping and directional drag oracle.
set -euo pipefail

repo_root=$(cd "$(dirname "$0")/../.." && pwd)
host_display=${WAYLAND_DISPLAY:-wayland-0}
log_dir=$(mktemp -d)
weston_pid=

module_map="crosswin-stage4.so=$repo_root/weston/build-stage4/crosswin/crosswin-stage4.so"
module_map+=";wayland-backend.so=$repo_root/weston/build-stage4/libweston/backend-wayland/wayland-backend.so"
module_map+=";desktop-shell.so=$repo_root/weston/build-stage4/desktop-shell/desktop-shell.so"
module_map+=";weston-desktop-shell=$repo_root/weston/build-stage4/clients/weston-desktop-shell"
module_map+=";weston-keyboard=$repo_root/weston/build-stage4/clients/weston-keyboard"

stop_weston() {
	if [[ -n ${weston_pid} ]]; then
		kill "${weston_pid}" 2>/dev/null || true
		wait "${weston_pid}" 2>/dev/null || true
		weston_pid=
	fi
}
trap stop_weston EXIT

start_weston() {
	local socket=$1
	local port=$2
	local output_x=$3
	local output_y=$4
	local scale=$5
	local initial_x=${6:-}
	local initial_y=${7:-}
	local log_file=$8
	local extra=()
	local attempt

	if [[ -n ${initial_x} ]]; then
		extra+=(--crosswin-initial-x="${initial_x}" --crosswin-initial-y="${initial_y}")
	fi
	WAYLAND_DISPLAY="${host_display}" WESTON_MODULE_MAP="${module_map}" \
		"${repo_root}/weston/build-stage4/frontend/weston" \
		--socket="${socket}" --backend=wayland-backend.so \
		--modules=crosswin-stage4.so --use-pixman --no-config \
		--crosswin-x="${output_x}" --crosswin-y="${output_y}" \
		--crosswin-scale-numerator="${scale%/*}" \
		--crosswin-scale-denominator="${scale#*/}" \
		--crosswin-export --crosswin-port="${port}" \
		--crosswin-trace-protocol --crosswin-trace-present \
		"${extra[@]}" >"${log_file}" 2>&1 &
	weston_pid=$!
	for attempt in $(seq 1 100); do
		grep -q "listening on 0.0.0.0:${port}" "${log_file}" && return 0
		sleep 0.05
	done
	echo "Weston did not start; see ${log_file}" >&2
	return 1
}

run_clip_case() {
	local case_name=$1
	local port=$2
	local output_x=$3
	local output_y=$4
	local scale=$5
	local initial_x=$6
	local initial_y=$7
	local socket="crosswin-7d-clip-${case_name}"
	local log_file="${log_dir}/${case_name}.log"

	start_weston "${socket}" "${port}" "${output_x}" "${output_y}" "${scale}" \
		"${initial_x}" "${initial_y}" "${log_file}"
	"${repo_root}/crosswin/build/stage7d-geometry-agent" "${port}" \
		--scale "${scale}" --case "${case_name}" &
	local agent_pid=$!
	WAYLAND_DISPLAY="${socket}" \
		"${repo_root}/weston/build-stage4/crosswin/crosswin-stage5-client" --run-ms 250
	wait "${agent_pid}"
	stop_weston
}

run_drag_case() {
	local placement=$1
	local port=$2
	local output_x=$3
	local output_y=$4
	local scale=$5
	local socket="crosswin-7d-drag-${placement}"
	local log_file="${log_dir}/drag-${placement}.log"

	start_weston "${socket}" "${port}" "${output_x}" "${output_y}" "${scale}" "" "" "${log_file}"
	"${repo_root}/crosswin/build/stage6-agent" "${port}" \
		--scale "${scale}" --placement "${placement}" &
	local agent_pid=$!
	WAYLAND_DISPLAY="${socket}" \
		"${repo_root}/weston/build-stage4/crosswin/crosswin-stage5-client" --run-ms 1000
	wait "${agent_pid}"
	stop_weston
}

if [[ ! -S "${XDG_RUNTIME_DIR:-}/${host_display}" ]]; then
	echo "host Wayland socket is unavailable: ${XDG_RUNTIME_DIR:-\$XDG_RUNTIME_DIR}/${host_display}" >&2
	exit 2
fi

run_clip_case right-half 44661 1024 0 1/1 624 80
run_clip_case right-one 44662 1024 0 1/1 225 80
run_clip_case left-half 44663 -1920 0 3/2 -400 80
run_clip_case left-one 44664 -1920 0 3/2 -1 80
run_clip_case above-half 44665 0 -1080 5/4 80 -300
run_clip_case above-one 44666 0 -1080 5/4 80 -1
run_clip_case below-half 44667 0 640 2/1 80 340
run_clip_case below-one 44668 0 640 2/1 80 41
run_drag_case right 44670 1024 0 1/1
run_drag_case left 44671 -1920 0 3/2

echo "stage7d logical geometry and directional drag integration: PASS"
echo "logs: ${log_dir}"
