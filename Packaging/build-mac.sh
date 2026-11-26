#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
dest="${1:-$HOME/Downloads/Hand Control}"
identity="${HAND_CONTROL_SIGN_IDENTITY:-}"
if [[ -z "$identity" && -e .git ]]; then
    identity="$(git config --local --default '-' --get handcontrol.signingIdentity)"
fi
identity="${identity:--}"
variant=development
if [[ "$identity" == '-' ]]; then variant=adhoc; fi
swift build -c release --product HandControl
bin="$(swift build -c release --show-bin-path)/HandControl"
bundle=".build/package-mac/$variant/Hand Control.app"
mkdir -p "$bundle/Contents/MacOS" "$dest"
install -m 755 "$bin" "$bundle/Contents/MacOS/HandControl"
xcrun strip -S "$bundle/Contents/MacOS/HandControl"
install -m 644 Packaging/Info.plist "$bundle/Contents/Info.plist"
codesign --force --sign "$identity" "$bundle"
codesign --verify --strict "$bundle"
ditto --norsrc --noextattr "$bundle" "$dest/Hand Control.app"
codesign --verify --strict "$dest/Hand Control.app"
printf 'Built %s/Hand Control.app (%s signing)\n' "$dest" "$variant"
