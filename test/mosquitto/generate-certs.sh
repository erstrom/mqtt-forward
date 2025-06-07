#!/bin/bash

SCRIPT_DIR=$(dirname "$(readlink -f "$0")")

# === CONFIG ===
CERT_DIR="mosquitto-certs"
CA_DIR="${CERT_DIR}/ca"
SERVER_DIR="${CERT_DIR}/server"
CLIENTS_DIR="${CERT_DIR}/clients"

cd "${SCRIPT_DIR}" || exit 1
mkdir -p "${CERT_DIR}" "${CA_DIR}" "${SERVER_DIR}" "${CLIENTS_DIR}"

# === 1. Generate CA (only once) ===
generate_ca() {
    if [[ -f "${CA_DIR}/ca.crt" ]]; then
        echo "CA already exists. Skipping."
    else
        echo "Generating CA..."
        openssl genrsa -out "${CA_DIR}/ca.key" 4096
        openssl req -x509 -new -nodes -key "${CA_DIR}/ca.key" -sha256 -days 1825 \
            -out "${CA_DIR}/ca.crt" -subj "/C=US/ST=State/L=City/O=MyOrg/CN=MyCA"
    fi
}

# === 2. Generate Server Certificate (only once) ===
generate_server_cert() {
    if [[ -f "${SERVER_DIR}/server.crt" ]]; then
        echo "Server certificate already exists. Skipping."
    else
        echo "Generating server certificate..."
        openssl genrsa -out "${SERVER_DIR}/server.key" 2048
        openssl req -new -key "${SERVER_DIR}/server.key" -out "${SERVER_DIR}/server.csr" \
            -subj "/C=US/ST=State/L=City/O=MyOrg/CN=localhost"

        cat > "${SERVER_DIR}/server.ext" <<EOF
authorityKeyIdentifier=keyid,issuer
basicConstraints=CA:FALSE
keyUsage = digitalSignature, nonRepudiation, keyEncipherment
extendedKeyUsage = serverAuth
subjectAltName = @alt_names

[alt_names]
DNS.1 = localhost
EOF

        openssl x509 -req -in "${SERVER_DIR}/server.csr" -CA "${CA_DIR}/ca.crt" \
            -CAkey "${CA_DIR}/ca.key" -CAcreateserial -out "${SERVER_DIR}/server.crt" \
            -days 825 -sha256 -extfile "${SERVER_DIR}/server.ext"
    fi
}

# === 3. Generate Client Certificate ===
generate_client_cert() {
    CLIENT_NAME="$1"
    if [[ -z "${CLIENT_NAME}" ]]; then
        echo "Usage: $0 client <client_name>"
        exit 1
    fi

    CLIENT_DIR="${CLIENTS_DIR}/${CLIENT_NAME}"
    mkdir -p "${CLIENT_DIR}"

    echo "Generating certificate for client: ${CLIENT_NAME}"
    openssl genrsa -out "${CLIENT_DIR}/${CLIENT_NAME}.key" 2048
    openssl req -new -key "${CLIENT_DIR}/${CLIENT_NAME}.key" -out "${CLIENT_DIR}/${CLIENT_NAME}.csr" \
        -subj "/C=US/ST=State/L=City/O=MyOrg/CN=${CLIENT_NAME}"

    openssl x509 -req -in "${CLIENT_DIR}/${CLIENT_NAME}.csr" -CA "${CA_DIR}/ca.crt" \
        -CAkey "${CA_DIR}/ca.key" -CAcreateserial -out "${CLIENT_DIR}/${CLIENT_NAME}.crt" \
        -days 825 -sha256

    echo "Client '${CLIENT_NAME}' certificate generated in: ${CLIENT_DIR}"
}

# === Command-line Interface ===
case "$1" in
  init)
    generate_ca
    generate_server_cert
    ;;
  client)
    generate_client_cert "$2"
    ;;
  *)
    echo "Usage:"
    echo "  $0 init               # Generate CA and server certificates"
    echo "  $0 client <name>      # Generate client certificate with given name"
    exit 1
    ;;
esac
