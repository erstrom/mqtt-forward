#! /bin/bash

set -eu

SCRIPT_DIR=$(dirname "$(readlink -f "$0")")

SECURE=1
while [[ "$#" -gt 0 ]]; do
	case "$1" in
		-u|--unsecure)
			SECURE=0
			shift
			;;
		*)
			echo "Unknown option: $1"
			exit 1
			;;
	esac
done

if [[ "${SECURE}" -eq 1 ]]; then
	MOSQUITTO_CONF="./mosquitto-secure.conf"
	# Create certificates
	"${SCRIPT_DIR}"/mosquitto/generate-certs.sh init
	"${SCRIPT_DIR}"/mosquitto/generate-certs.sh client client-side
	"${SCRIPT_DIR}"/mosquitto/generate-certs.sh client server-side
else
	MOSQUITTO_CONF="./mosquitto-unsecure.conf"
fi

# Create environment file for docker-compose
UID_GID="$(id -u)":"$(id -g)"
echo "UID_GID=${UID_GID}" > "${SCRIPT_DIR}"/mosquitto/.env
echo "MOSQUITTO_CONF=${MOSQUITTO_CONF}" >> "${SCRIPT_DIR}"/mosquitto/.env
