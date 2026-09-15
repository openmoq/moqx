#!/usr/bin/env bash
# Central QUIC-stack selection for the shell integration tests.
#
# MOQ_HARNESS_QUIC_STACK chooses the listener stack: mvfst (the default, so an
# unset environment behaves exactly as before) or picoquic. The Python topology
# harness reads the same variable, so one export flips the whole suite.
#
# Source this file; it defines functions only.

MOQ_QUIC_STACK="${MOQ_HARNESS_QUIC_STACK:-mvfst}"
case "$MOQ_QUIC_STACK" in
  mvfst | picoquic) ;;
  *)
    echo "MOQ_HARNESS_QUIC_STACK must be mvfst or picoquic, got '$MOQ_QUIC_STACK'" >&2
    exit 1
    ;;
esac

# Writes the stack and tls keys of a listeners[] entry, indented to sit under
# `- name:`. Picoquic rejects `insecure: true`, so it serves a self-signed
# localhost cert generated into $1, the caller's temp directory.
moq_listener_stack_yaml() {
  if [[ "$MOQ_QUIC_STACK" != picoquic ]]; then
    printf '    tls:\n      insecure: true\n'
    return
  fi
  local dir="${1:?moq_listener_stack_yaml needs a directory to hold the generated cert}"
  local cert="$dir/moqx-test-cert.pem"
  local key="$dir/moqx-test-key.pem"
  if [[ ! -f "$cert" ]]; then
    openssl req -x509 -newkey rsa:2048 -nodes \
      -keyout "$key" -out "$cert" -days 1 \
      -subj "/CN=localhost" -addext "subjectAltName=DNS:localhost" \
      >/dev/null 2>&1 || {
      echo "openssl could not generate a test cert in $dir" >&2
      exit 1
    }
  fi
  printf '    quic_stack: picoquic\n    tls:\n      insecure: false\n      cert_file: "%s"\n      key_file: "%s"\n' \
    "$cert" "$key"
}

# Writes the services[].match path entry. Picoquic's WebTransport endpoint
# table only matches exact paths; under a prefix it registers no endpoints at
# all and rejects every connection.
moq_service_path_yaml() {
  if [[ "$MOQ_QUIC_STACK" = picoquic ]]; then
    printf '        path: {exact: "/moq-relay"}\n'
  else
    printf '        path: {prefix: "/"}\n'
  fi
}
