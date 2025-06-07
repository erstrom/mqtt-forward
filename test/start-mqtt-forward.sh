#! /bin/bash

SCRIPT_DIR=$(dirname "$(readlink -f "$0")")

SECURE=1
SERVER_SIDE=0

while [[ "$#" -gt 0 ]]; do
	case "$1" in
		-u|--unsecure)
			SECURE=0
			shift
			;;
		-s|--server-side)
			SERVER_SIDE=1
			shift
			;;
		*)
			echo "Unknown option: $1"
			exit 1
			;;
	esac
done

SERVER_SIDE_ID=mqtt-forward-test
MQTT_FORWARD=$(readlink -f "${SCRIPT_DIR}"/../build/mqtt-forward)
CA="${SCRIPT_DIR}"/mosquitto/mosquitto-certs/ca/ca.crt
if [[ "${SERVER_SIDE}" -eq 1 ]]; then
	CERT="${SCRIPT_DIR}"/mosquitto/mosquitto-certs/clients/server-side/server-side.crt
	PRIVATE_KEY="${SCRIPT_DIR}"/mosquitto/mosquitto-certs/clients/server-side/server-side.key
	PORT=22
	SERVER_SIDE_ARGS="-b --server "
else
	CERT="${SCRIPT_DIR}"/mosquitto/mosquitto-certs/clients/client-side/client-side.crt
	PRIVATE_KEY="${SCRIPT_DIR}"/mosquitto/mosquitto-certs/clients/client-side/client-side.key
	PORT=1230
	SERVER_SIDE_ARGS=""
fi

if [[ ! -f "${MQTT_FORWARD}" ]] ; then
	echo "mqtt-forward binary: ${MQTT_FORWARD} is missing. Is it built?"
	exit 1
fi

SECURE_ARGS=""
if [[ "${SECURE}" -eq 1 ]]; then
	if [[ ! -f "${CA}" ]] ; then
		echo "File ${CA} is missing. Has the certificates been generated?"
		exit 1
	fi

	if [[ ! -f "${CERT}" ]] ; then
		echo "File ${CERT} is missing. Has the certificates been generated?"
		exit 1
	fi

	if [[ ! -f "${PRIVATE_KEY}" ]] ; then
		echo "File ${PRIVATE_KEY} is missing. Has the certificates been generated?"
		exit 1
	fi
	SECURE_ARGS="--tls --mqtt-root-ca ${CA} --mqtt-certificate ${CERT} --mqtt-private-key ${PRIVATE_KEY} "
fi

"${MQTT_FORWARD}" --server-side-id "${SERVER_SIDE_ID}" \
${SECURE_ARGS} \
${SERVER_SIDE_ARGS} \
-p "${PORT}" \
--mqtt-host localhost
