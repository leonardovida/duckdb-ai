#!/bin/bash

# Extension upload script

# Usage: ./extension-upload.sh <name> <extension_version> <duckdb_version> <architecture> <s3_bucket> <copy_to_latest> <copy_to_versioned>
# <name>                : Name of the extension
# <extension_version>   : Version (commit / version tag) of the extension
# <duckdb_version>      : Version (commit / version tag) of DuckDB
# <architecture>        : Architecture target of the extension binary
# <s3_bucket>           : S3 bucket to upload to
# <copy_to_latest>      : Set this as the latest version ("true" / "false", default: "false")
# <copy_to_versioned>   : Set this as a versioned version that will prevent its deletion

set -euo pipefail

if (( $# < 5 || $# > 7 )); then
  echo "Usage: $0 <name> <extension_version> <duckdb_version> <architecture> <s3_bucket> [copy_to_latest] [copy_to_versioned]" >&2
  exit 1
fi

name=$1
extension_version=$2
duckdb_version=$3
architecture=$4
bucket=$5
copy_to_latest=${6:-false}
copy_to_versioned=${7:-false}
if [[ ! $name =~ ^[a-zA-Z0-9_-]+$ ]] ||
   [[ $copy_to_latest != true && $copy_to_latest != false ]] ||
   [[ $copy_to_versioned != true && $copy_to_versioned != false ]]; then
  echo "Invalid extension name or upload flags" >&2
  exit 1
fi

ext="/tmp/extension/$name.duckdb_extension"
if [[ $architecture == wasm* ]]; then
  ext+=".wasm"
fi

echo "$ext"
script_dir="$(dirname "$(readlink -f "$0")")"

# Keep private keys and the hash helper's x* intermediates out of the caller's
# working directory. Always remove staging files, including on signing failure.
umask 077
staging_dir=$(mktemp -d /tmp/duckdb-ai-upload.XXXXXXXX)
trap 'rm -rf -- "$staging_dir"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
staged="$staging_dir/extension"
cp -- "$ext" "$staged.append"

if [[ $architecture == wasm* ]]; then
  # WebAssembly custom section: name + length + the 256-byte RSA signature.
  printf '\x00\x93\x02\x10duckdb_signature\x80\x02' >> "$staged.append"
fi

if [[ -n ${DUCKDB_EXTENSION_SIGNING_PK:-} ]]; then
  printf '%s\n' "$DUCKDB_EXTENSION_SIGNING_PK" > "$staging_dir/private.pem"
  (
    cd "$staging_dir"
    "$script_dir/../duckdb/scripts/compute-extension-hash.sh" "$staged.append" > "$staged.hash"
  )
  openssl pkeyutl -sign -in "$staged.hash" -inkey "$staging_dir/private.pem" -pkeyopt digest:sha256 -out "$staged.sign"
  if [[ $(wc -c < "$staged.sign") -ne 256 ]]; then
    echo "DuckDB extensions require a 256-byte RSA signature" >&2
    exit 1
  fi
else
  # A fresh staging file prevents unsigned reruns from retaining old signatures.
  truncate -s 256 "$staged.sign"
fi
cat "$staged.sign" >> "$staged.append"

if [[ $architecture == wasm* ]]; then
  brotli < "$staged.append" > "$staged.compressed"
else
  gzip < "$staged.append" > "$staged.compressed"
fi

# Preserve the existing sidecar paths only after packaging succeeds.
for suffix in append sign compressed; do
  cp -- "$staged.$suffix" "$ext.$suffix"
done
if [[ -f $staged.hash ]]; then
  cp -- "$staged.hash" "$ext.hash"
else
  rm -f -- "$ext.hash"
fi

if [[ -z ${AWS_ACCESS_KEY_ID:-} ]]; then
  echo "No AWS key found, skipping.."
  exit 0
fi

upload() {
  local destination=$1
  if [[ $architecture == wasm* ]]; then
    aws s3 cp "$ext.compressed" "$destination/$name.duckdb_extension.wasm" --acl public-read --content-encoding br --content-type="application/wasm"
  else
    aws s3 cp "$ext.compressed" "$destination/$name.duckdb_extension.gz" --acl public-read
  fi
}

if [[ $copy_to_versioned == true ]]; then
  upload "s3://$bucket/$name/$extension_version/$duckdb_version/$architecture"
fi
if [[ $copy_to_latest == true ]]; then
  upload "s3://$bucket/$duckdb_version/$architecture"
fi
