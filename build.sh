#!/usr/bin/env bash
# Puts @bartlwrs/mln-next-web, the map package this page uses, into
# web/vendor/mln-next-web: from npm ($MLN_NEXT_WEB_VERSION, latest by
# default), or from a local build of it:
#
#   ./build.sh [path/to/maplibre-native-next/platform/web/dist]
#
# The package starts its workers and loads its WebAssembly from its own
# directory, so it has to be served from this page's origin rather than from a
# CDN.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

vendor=web/vendor/mln-next-web
rm -rf "$vendor"
mkdir -p "$vendor"
if [[ $# -gt 0 ]]; then
  cp -R "$1"/. "$vendor"/
else
  mkdir -p build
  tarball=$(cd build && npm pack --silent "@bartlwrs/mln-next-web@${MLN_NEXT_WEB_VERSION:-latest}")
  tar -xzf "build/$tarball" -C "$vendor" --strip-components=1
fi
echo "web/vendor/mln-next-web: $(node -p "require('./$vendor/package.json').version")"
