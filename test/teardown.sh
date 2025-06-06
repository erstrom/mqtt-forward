#! /bin/bash

# kill all mqtt-forward instances
killall mqtt-forward

# Kill mosquitto broker
MOSQUITTO_PID_FILE="/tmp/mosquitto-mqtt-forward-test.pid"

if [[ -f "${MOSQUITTO_PID_FILE}" ]]; then
    PID=$(cat "${MOSQUITTO_PID_FILE}")
    if kill -0 "${PID}" 2>/dev/null; then
        kill "${PID}"
        echo "Mosquitto (PID ${PID}) terminated."
        rm "${MOSQUITTO_PID_FILE}"
    else
        echo "Process with PID ${PID} is not running."
        rm "${MOSQUITTO_PID_FILE}"
    fi
else
    echo "Mosquitto broker PID file not found."
fi
