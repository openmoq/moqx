#!/bin/sh
# Translate ORLY_CERT / ORLY_KEY env vars to --cert / --key flags,
# then exec o-rly with any additional arguments passed to the container.
set -e
exec /usr/local/bin/o-rly \
    ${ORLY_CERT:+--cert="$ORLY_CERT"} \
    ${ORLY_KEY:+--key="$ORLY_KEY"} \
    "$@"
