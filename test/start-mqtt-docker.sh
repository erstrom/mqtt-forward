#! /bin/bash

SCRIPT_DIR=$(dirname "$(readlink -f "$0")")

if [[ ! -f "${SCRIPT_DIR}"/mosquitto/.env ]] ; then
	echo "Missing ${SCRIPT_DIR}/mosquitto/.env. Run setup-mqtt-docker.sh"
	exit 0
fi

cd "${SCRIPT_DIR}"/mosquitto || exit 1

docker-compose up -d
