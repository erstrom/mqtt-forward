#! /bin/bash

SCRIPT_DIR=$(dirname $(readlink -f $0))

# Start mosquitto
$SCRIPT_DIR/mosquitto/run-mosquitto.sh

# Start server side program
$SCRIPT_DIR/run-server-side.sh &

# Start client side program
$SCRIPT_DIR/run-client-side.sh &
