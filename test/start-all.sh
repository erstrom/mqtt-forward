#! /bin/bash

set -eu

SCRIPT_DIR=$(dirname "$(readlink -f "$0")")

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

# Start server side program
"${SCRIPT_DIR}"/start-mqtt-forward.sh -s ${UNSECURE} &

# Start client side program
"${SCRIPT_DIR}"/start-mqtt-forward.sh ${UNSECURE} &
