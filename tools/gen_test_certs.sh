#!/usr/bin/env bash
# Throwaway test certificates for the Step 5b.4 wss device matrix (http_check.py --tls-test).
#
#   tools/gen_test_certs.sh <pc-ip> [outdir]        (Git Bash, needs openssl)
#
# Default outdir is .pio/testcerts/ (gitignored). NEVER commit the output: it contains private
# keys. The keys are throwaway test keys, but the repo must stay free of any key material.
#
# Output (PEM; *.key are the private keys, only the mock server needs them):
#   ca-a.pem / ca-a.key    CA-A  (CA:TRUE, keyCertSign, RSA-2048)
#   ca-b.pem / ca-b.key    CA-B  (an unrelated CA, RSA-2048)
#   ca-c.pem               a third CA (RSA-3072), only used for bigbundle.pem
#   cn-ip.pem / .key       leaf CN=<pc-ip>, no SAN, signed by CA-A, RSA-2048  -> verifies (T1)
#   san-ip.pem / .key      leaf CN=<pc-ip> + SAN IP:<pc-ip>, signed by CA-A    -> rejected (T5, F11)
#   self.pem / self.key    self-signed leaf CN=<pc-ip>, no SAN                  -> as CA (T6)
#   bigbundle.pem          CA-A + CA-B + CA-C, normalised length <= 4096 (heap gate)
#   bad-ca.pem             structurally valid base64 CERTIFICATE block that is no certificate (T10)
#   lengths.txt            normalised PEM lengths (chars)
set -euo pipefail
export MSYS_NO_PATHCONV=1   # Git Bash must not rewrite "/CN=..." into a Windows path

ip="${1:?usage: gen_test_certs.sh <pc-ip> [outdir]}"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
out="${2:-$here/../.pio/testcerts}"
mkdir -p "$out"
out="$(cd "$out" && pwd)"
cd "$out"

days=3650
ext() { printf '%s\n' "$@" > ext.tmp; }

new_ca() {  # name bits [extra-subject-fields]
    openssl req -x509 -newkey "rsa:$2" -nodes -keyout "$1.key" -out "$1.pem" -days $days \
        -subj "/CN=HAKnob Test $1${3:-}" \
        -addext "basicConstraints=critical,CA:TRUE" -addext "keyUsage=critical,keyCertSign,cRLSign" \
        2>/dev/null
}

new_leaf() {  # name signer sanline-or-empty
    openssl req -new -newkey rsa:2048 -nodes -keyout "$1.key" -out "$1.csr" -subj "/CN=$ip" 2>/dev/null
    if [ -n "$3" ]; then ext "basicConstraints=CA:FALSE" "keyUsage=digitalSignature,keyEncipherment" "$3"
    else ext "basicConstraints=CA:FALSE" "keyUsage=digitalSignature,keyEncipherment"; fi
    openssl x509 -req -in "$1.csr" -CA "$2.pem" -CAkey "$2.key" -CAcreateserial -out "$1.pem" \
        -days $days -extfile ext.tmp 2>/dev/null
    rm -f "$1.csr"
}

new_ca ca-a 2048
new_ca ca-b 2048
# ca-c carries long subject fields so that bigbundle.pem lands just below 4096 chars
pad38() { printf "$1%.0s" $(seq 1 38); }
new_ca ca-c 3072 "/O=$(pad38 o)/OU=$(pad38 u)"
rm -f ca-c.key    # never needed

new_leaf cn-ip ca-a ""
new_leaf san-ip ca-a "subjectAltName=IP:$ip"

# self-signed plain leaf (CA:FALSE), CN=<ip>, no SAN
openssl req -x509 -newkey rsa:2048 -nodes -keyout self.key -out self.pem -days $days -subj "/CN=$ip" \
    -addext "basicConstraints=CA:FALSE" -addext "keyUsage=digitalSignature,keyEncipherment" 2>/dev/null

cat ca-a.pem ca-b.pem ca-c.pem > bigbundle.pem

# structurally valid but unparsable: DER 30 06 30 04 02 02 00 01 (a SEQUENCE holding a SEQUENCE
# holding a short INTEGER), base64 in CERTIFICATE armor
{
    echo "-----BEGIN CERTIFICATE-----"
    printf '\x30\x06\x30\x04\x02\x02\x00\x01' | openssl base64 -A
    echo
    echo "-----END CERTIFICATE-----"
} > bad-ca.pem

rm -f ext.tmp ./*.srl

: > lengths.txt
for f in ca-a ca-b ca-c cn-ip san-ip self bigbundle bad-ca; do
    # normalised like the firmware: LF only, trimmed, one trailing LF
    n=$(tr -d '\r' < "$f.pem" | sed -e :a -e '/^\n*$/{$d;N;ba' -e '}' | wc -c)
    echo "$f.pem $n" | tee -a lengths.txt
done
big=$(awk '$1=="bigbundle.pem"{print $2}' lengths.txt)
if [ "$big" -gt 4096 ]; then echo "bigbundle.pem is $big chars (> 4096)" >&2; exit 1; fi
echo "test certificates for $ip written to $out (do not commit)"
