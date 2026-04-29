#!/usr/bin/env bash
set -euo pipefail

PACKAGE="${1:-}"

if [[ -z "$PACKAGE" || ! -f "$PACKAGE" ]]; then
  echo "Package not found: $PACKAGE" >&2
  exit 1
fi

TARGET_BASE=""
CANDIDATES=(
  "$HOME/Library/Application Support/TeamSpeak 3"
  "$HOME/Library/Application Support/TS3Client"
  "$HOME/.ts3client"
)

for candidate in "${CANDIDATES[@]}"; do
  if [[ -d "$candidate" ]]; then
    TARGET_BASE="$candidate"
    break
  fi
done

if [[ -z "$TARGET_BASE" ]]; then
  TARGET_BASE="$HOME/Library/Application Support/TeamSpeak 3"
fi

PLUGIN_DIR="$TARGET_BASE/plugins"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

mkdir -p "$PLUGIN_DIR"
/usr/bin/ditto -x -k "$PACKAGE" "$TMP_DIR"

if [[ ! -d "$TMP_DIR/plugins" ]]; then
  echo "Invalid .ts3_plugin package: plugins/ folder is missing." >&2
  exit 1
fi

/usr/bin/osascript -e 'tell application "TeamSpeak 3" to quit' >/dev/null 2>&1 || true
sleep 1

rm -f "$PLUGIN_DIR/librp_soundboard_fx_mac.dylib" \
      "$PLUGIN_DIR/librp_soundboard_fx_mac.so" \
      "$PLUGIN_DIR/rp_soundboard_fx_mac.dylib" \
      "$PLUGIN_DIR/rp_soundboard_fx_mac.so"

# Wipe every bundled dep .dylib from the prior install so we don't end
# up with stale FFmpeg/transitive libs alongside fresh ones.
rm -f "$PLUGIN_DIR"/lib*.dylib 2>/dev/null || true

cp -R "$TMP_DIR/plugins/." "$PLUGIN_DIR/"

find "$PLUGIN_DIR" -maxdepth 1 -name 'librp_soundboard_fx*.dylib' -exec xattr -dr com.apple.quarantine {} + >/dev/null 2>&1 || true
find "$PLUGIN_DIR" -maxdepth 1 -name 'libav*.dylib'              -exec xattr -dr com.apple.quarantine {} + >/dev/null 2>&1 || true
find "$PLUGIN_DIR" -maxdepth 1 -name 'libsw*.dylib'              -exec xattr -dr com.apple.quarantine {} + >/dev/null 2>&1 || true

echo "$PLUGIN_DIR"
