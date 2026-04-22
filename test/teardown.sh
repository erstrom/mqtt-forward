#! /bin/bash

set -eu

SCRIPT_DIR=$(dirname "$(readlink -f "$0")")
PID_DIR="${SCRIPT_DIR}/.pids"

cd "${SCRIPT_DIR}"/mosquitto || exit 1

# Stop mosquitto broker container
docker compose down

# Stop mqtt-forward instances started by start-all.sh
for pid_file in "${PID_DIR}"/*.pid; do
	[[ -f "${pid_file}" ]] || continue

	pid=$(cat "${pid_file}")
	if kill -0 "${pid}" 2>/dev/null &&
	   [[ "$(ps -p "${pid}" -o comm=)" == "mqtt-forward" ]]; then
		kill "${pid}"
	fi

	rm -f "${pid_file}"
done
