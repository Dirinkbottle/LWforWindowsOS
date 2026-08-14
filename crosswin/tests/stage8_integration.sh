#!/usr/bin/env bash
# Nested-Weston end-to-end oracle for Stage 8 window-scoped keyboard + wheel.
set -euo pipefail

repo_root=$(cd "$(dirname "$0")/../.." && pwd)
host_display=${WAYLAND_DISPLAY:-wayland-0}
log_dir=$(mktemp -d)
port=44680
socket=crosswin-stage8-auto
weston_pid=
agent_pid=

module_map="crosswin-stage4.so=$repo_root/weston/build-stage4/crosswin/crosswin-stage4.so"
module_map+=";wayland-backend.so=$repo_root/weston/build-stage4/libweston/backend-wayland/wayland-backend.so"
module_map+=";desktop-shell.so=$repo_root/weston/build-stage4/desktop-shell/desktop-shell.so"
module_map+=";weston-desktop-shell=$repo_root/weston/build-stage4/clients/weston-desktop-shell"
module_map+=";weston-keyboard=$repo_root/weston/build-stage4/clients/weston-keyboard"

stop() {
	if [[ -n ${agent_pid} ]]; then
		kill "${agent_pid}" 2>/dev/null || true
		wait "${agent_pid}" 2>/dev/null || true
	fi
	if [[ -n ${weston_pid} ]]; then
		kill "${weston_pid}" 2>/dev/null || true
		wait "${weston_pid}" 2>/dev/null || true
	fi
}
trap stop EXIT

if [[ ! -S "${XDG_RUNTIME_DIR:-}/${host_display}" ]]; then
	echo "host Wayland socket is unavailable: ${XDG_RUNTIME_DIR:-\$XDG_RUNTIME_DIR}/${host_display}" >&2
	exit 2
fi

WAYLAND_DISPLAY="${host_display}" WESTON_MODULE_MAP="${module_map}" \
	"${repo_root}/weston/build-stage4/frontend/weston" \
	--socket="${socket}" --backend=wayland-backend.so \
	--modules=crosswin-stage4.so --use-pixman --no-config \
	--crosswin-x=1024 --crosswin-export --crosswin-port="${port}" \
	--crosswin-trace-protocol --crosswin-trace-input >"${log_dir}/weston.log" 2>&1 &
weston_pid=$!
for _ in $(seq 1 100); do
	grep -q "listening on 0.0.0.0:${port}" "${log_dir}/weston.log" && break
	sleep 0.05
done
grep -q "listening on 0.0.0.0:${port}" "${log_dir}/weston.log"

"${repo_root}/crosswin/build/stage8-agent" "${port}" >"${log_dir}/agent.log" 2>&1 &
agent_pid=$!
WAYLAND_DISPLAY="${socket}" \
	"${repo_root}/weston/build-stage4/crosswin/crosswin-stage5-client" \
	--run-ms 1500 --expect-input >"${log_dir}/client.log" 2>&1
wait "${agent_pid}"
agent_pid=
grep -q "event=wheel .* delivered" "${log_dir}/weston.log"
grep -q "event=keyboard-key .* key=30 state=1" "${log_dir}/weston.log"

echo "stage8 window-scoped keyboard/wheel integration: PASS"
echo "logs: ${log_dir}"
