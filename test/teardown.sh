#! /bin/bash

set -eu

SCRIPT_DIR=$(dirname "$(readlink -f "$0")")

cd "${SCRIPT_DIR}"/mosquitto || exit 1

# Stop mosquitto broker container
docker-compose down

# kill all mqtt-forward instances
killall mqtt-forward
