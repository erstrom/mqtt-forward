#! /bin/bash

SCRIPT_DIR=$(dirname $(readlink -f $0))

SERVER_SIDE_ID=mqtt-forward-test
MQTT_FORWARD=$(readlink -f $SCRIPT_DIR/../build/mqtt-forward)

 $MQTT_FORWARD --server-side-id $SERVER_SIDE_ID \
-b \
--server \
-p 22 \
--mqtt-host localhost
