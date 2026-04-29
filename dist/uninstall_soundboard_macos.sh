#!/usr/bin/env bash
# ============================================================
#  GameBaiters Soundboard - Uninstaller (macOS)
# ============================================================
set -u

CANDIDATES=(
    "${HOME}/Library/Application Support/TS3Client"
    "${HOME}/.ts3client"
)

TS3_BASE=""
for p in "${CANDIDATES[@]}"; do
    if [[ -d "$p" ]]; then TS3_BASE="$p"; break; fi
done

STAMP="$(date +%Y%m%d_%H%M%S)"
BACKUP_DIR="${HOME}/Desktop/GameBaiters_Soundboard_Backup_${STAMP}"

errors=0
delete_config="N"
do_backup="Y"

echo
echo "  GameBaiters Soundboard - Uninstaller (macOS)"
echo "  ============================================"
echo
echo "  This script removes the plugin from TeamSpeak 3."
echo "  Default mode preserves rp_soundboard.ini so a fresh"
echo "  install reuses your existing soundboard layout."
echo

# Prompt 1: delete config?
echo "  Delete config (rp_soundboard.ini) too?"
echo "    [N] No, keep config (default - safe for upgrade)"
echo "    [Y] Yes, full wipe"
echo
read -rp "  Delete config? (N/Y, default N): " del_in
del_in="${del_in:-N}"
delete_config="$(printf '%s' "${del_in:0:1}" | tr '[:lower:]' '[:upper:]')"
echo

# Prompt 2: backup?
echo "  Backup before deleting?"
echo "    [Y] Yes (default)"
echo "    [N] No"
echo
read -rp "  Create backup? (Y/N, default Y): " bak_in
bak_in="${bak_in:-Y}"
do_backup="$(printf '%s' "${bak_in:0:1}" | tr '[:lower:]' '[:upper:]')"
echo

# Step 1: kill TS3
if pgrep -x ts3client >/dev/null 2>&1 || pgrep -if "TeamSpeak 3" >/dev/null 2>&1; then
    read -rp "  TeamSpeak 3 is running. Quit it? (y/N): " ans
    if [[ "${ans:0:1}" == "y" || "${ans:0:1}" == "Y" ]]; then
        osascript -e 'tell application "TeamSpeak 3" to quit' 2>/dev/null
        sleep 2
        pkill -9 -x ts3client 2>/dev/null
        pkill -9 -if "TeamSpeak 3" 2>/dev/null
        echo "  [OK] TeamSpeak 3 terminated."
    fi
fi

if [[ -z "${TS3_BASE}" ]]; then
    echo "  [INFO] No TS3 base directory found - nothing to do."
    echo
    read -rp "  Press Enter to close..." _
    exit 0
fi

TS3_PLUGINS="${TS3_BASE}/plugins"
TS3_SETTINGS="${TS3_BASE}/settings.db"
TS3_CONFIG="${TS3_BASE}/rp_soundboard.ini"

# Step 2: backup
if [[ "${do_backup}" == "Y" ]]; then
    echo
    echo "  TS3 base: ${TS3_BASE}"
    echo "  Backup to ${BACKUP_DIR} ..."
    mkdir -p "${BACKUP_DIR}"
    [[ -d "${TS3_PLUGINS}" ]] && cp -a "${TS3_PLUGINS}" "${BACKUP_DIR}/plugins" 2>/dev/null
    [[ -f "${TS3_SETTINGS}" ]] && cp -a "${TS3_SETTINGS}" "${BACKUP_DIR}/settings.db.bak" 2>/dev/null
    [[ -f "${TS3_CONFIG}" ]] && cp -a "${TS3_CONFIG}" "${BACKUP_DIR}/rp_soundboard.ini.bak" 2>/dev/null
    echo "  [OK] Backup created."
else
    echo "  [SKIP] Backup skipped per user choice."
fi

# Step 3: delete plugin libs
echo
echo "  Removing plugin libraries..."
for lib in \
    librp_soundboard_fx.dylib librp_soundboard.dylib libsoundboard.dylib \
    rp_soundboard_fx.dylib    rp_soundboard.dylib    soundboard.dylib    \
    librp_soundboard_fx.so    librp_soundboard.so    libsoundboard.so
do
    if [[ -f "${TS3_PLUGINS}/${lib}" ]]; then
        rm -f "${TS3_PLUGINS}/${lib}" && echo "  [OK] Deleted ${lib}" || { echo "  [FAIL] ${lib}"; errors=$((errors+1)); }
    fi
done

# Step 4: asset folders
echo
echo "  Removing asset folders..."
for d in rp_soundboard rp_soundboard_fx soundboard; do
    if [[ -d "${TS3_PLUGINS}/${d}" ]]; then
        rm -rf "${TS3_PLUGINS}/${d}" && echo "  [OK] Removed ${d}/" || { echo "  [FAIL] ${d}/"; errors=$((errors+1)); }
    fi
done

# Step 5: logs always; config only if opted in
echo
echo "  Removing logs..."
rm -f "${TS3_BASE}/rpsb_debug.log"     2>/dev/null
rm -f "${TS3_BASE}"/rp_soundboard*.log 2>/dev/null
rm -f "${TS3_BASE}"/soundboard*.log    2>/dev/null
echo "  [OK] Logs swept."

if [[ "${delete_config}" == "Y" ]]; then
    if [[ -f "${TS3_CONFIG}" ]]; then
        rm -f "${TS3_CONFIG}"
        echo "  [OK] Removed rp_soundboard.ini."
    fi
else
    if [[ -f "${TS3_CONFIG}" ]]; then
        echo "  [KEEP] rp_soundboard.ini preserved (re-used on next install)."
    fi
fi

# Step 6: settings.db
echo
echo "  Cleaning settings database entries..."
if command -v sqlite3 >/dev/null 2>&1; then
    if [[ -f "${TS3_SETTINGS}" ]]; then
        sqlite3 "${TS3_SETTINGS}" \
          "DELETE FROM Plugins WHERE value LIKE '%rp_soundboard%' OR value LIKE '%soundboard%';" 2>/dev/null
        sqlite3 "${TS3_SETTINGS}" "VACUUM;" 2>/dev/null
        echo "  [OK] settings.db cleaned and vacuumed."
    else
        echo "  [SKIP] settings.db not found."
    fi
else
    echo "  [WARN] sqlite3 not installed - settings.db NOT cleaned."
fi

# Step 7: configs (only if full wipe)
if [[ "${delete_config}" == "Y" ]]; then
    [[ -d "${TS3_BASE}/rp_soundboard_configs" ]] && { rm -rf "${TS3_BASE}/rp_soundboard_configs"; echo "  [OK] Removed rp_soundboard_configs/"; }
fi

echo
echo "  ============================================"
if (( errors == 0 )); then
    echo "  Uninstall complete."
    [[ "${delete_config}" == "N" ]] && echo "  Config preserved at ${TS3_CONFIG}"
else
    echo "  Uninstall finished with ${errors} error(s)."
fi
[[ "${do_backup}" == "Y" ]] && echo "  Backup directory: ${BACKUP_DIR}"
echo "  ============================================"
echo
read -rp "  Press Enter to close..." _
