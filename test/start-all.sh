#! /bin/bash

set -eu

SCRIPT_DIR=$(dirname "$(readlink -f "$0")")
PID_DIR="${SCRIPT_DIR}/.pids"

UNSECURE=""
while [[ "$#" -gt 0 ]]; do
	case "$1" in
		-u|--unsecure)
			UNSECURE="--unsecure"
			shift
			;;
		*)
			echo "Unknown option: $1"
			exit 1
			;;
	esac
done

# Start mosquitto
"${SCRIPT_DIR}"/setup-mqtt-docker.sh ${UNSECURE}
"${SCRIPT_DIR}"/start-mqtt-docker.sh

mkdir -p "${PID_DIR}"
rm -f "${PID_DIR}"/*.pid

# Start server side program
"${SCRIPT_DIR}"/start-mqtt-forward.sh -s ${UNSECURE} &
echo "$!" > "${PID_DIR}/mqtt-forward-server.pid"

# Start client side program
"${SCRIPT_DIR}"/start-mqtt-forward.sh ${UNSECURE} &
echo "$!" > "${PID_DIR}/mqtt-forward-client.pid"
