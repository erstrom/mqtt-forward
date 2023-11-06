#! /bin/bash

SCRIPT_DIR=$(dirname $(readlink -f $0))

/usr/sbin/mosquitto -c $SCRIPT_DIR/mosquitto.conf -d
