#! /bin/bash

set -eu

SCRIPT_DIR=$(dirname "$(readlink -f "$0")")

CHECKPATCH="${SCRIPT_DIR}"/checkpatch/checkpatch.pl
SRC_DIR="${SCRIPT_DIR}"/../src

if [[ ! -f "${CHECKPATCH}" ]] ; then
	echo "Can't find ${CHECKPATCH}. Please make sure checkpatch sub module is initialized"
	exit 1
fi

"${CHECKPATCH}" -f "${SRC_DIR}"/*
