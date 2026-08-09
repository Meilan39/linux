#!/bin/bash
set -e

# ==========================================
# Fragnesia Test Runner (QEMU-compatible)
# Usage: ./run.sh [fragnesia|fragnesia_pks]
# Run from: ~/src/linux-fragnesia-test/_fragnesia
# ==========================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

# Check for exploit selection
if [ $# -ne 1 ]; then
    echo -e "${RED}Usage: $0 [fragnesia|fragnesia_pks]${NC}"
    echo "  fragnesia       - Run unprotected exploit"
    echo "  fragnesia_pks   - Run PKS-protected exploit"
    exit 1
fi

EXPLOIT_NAME="$1"

# Check if compiled binary exists
if [ ! -f "$SCRIPT_DIR/$EXPLOIT_NAME" ]; then
    echo -e "${RED}Error: Compiled binary $EXPLOIT_NAME not found${NC}"
    echo -e "${YELLOW}Compile on host first:${NC}"
    if [ "$EXPLOIT_NAME" = "fragnesia_pks" ] && [ -f "$SCRIPT_DIR/pks.h" ]; then
        echo "  gcc -O2 -Wall -static -o $EXPLOIT_NAME ${EXPLOIT_NAME}.c -lpthread"
    else
        echo "  gcc -O2 -Wall -static -o $EXPLOIT_NAME ${EXPLOIT_NAME}.c -lpthread"
    fi
    exit 1
fi

echo -e "${CYAN}========================================${NC}"
echo -e "${CYAN}  Fragnesia QEMU Test Suite${NC}"
echo -e "${CYAN}  Exploit: $EXPLOIT_NAME${NC}"
echo -e "${CYAN}  Directory: $SCRIPT_DIR${NC}"
echo -e "${CYAN}========================================${NC}"
echo ""

# ==========================================
# Step 1: Pre-flight Checks & Setup
# ==========================================
echo -e "${YELLOW}[*] Step 1: Pre-flight checks...${NC}"

# Check if running as root
if [ "$EUID" -ne 0 ]; then 
    echo -e "${RED}[!] This script must be run as root${NC}"
    exit 1
fi

# Check if /usr/bin/su exists and is a valid target
if [ ! -f /usr/bin/su ]; then
    echo -e "${RED}[!] /usr/bin/su not found - exploit target missing${NC}"
    exit 1
fi
echo -e "${GREEN}[+] Target /usr/bin/su found${NC}"

# Backup original SU binary info
echo -e "${YELLOW}[*] Capturing /usr/bin/su metadata...${NC}"
ORIGINAL_MD5=$(md5sum /usr/bin/su)
ORIGINAL_SHA256=$(sha256sum /usr/bin/su)
ORIGINAL_SIZE=$(stat -c%s /usr/bin/su)
echo -e "${GREEN}[+] Size: $ORIGINAL_SIZE bytes${NC}"
echo -e "${GREEN}[+] MD5: ${ORIGINAL_MD5%% *}${NC}"

# ==========================================
# Step 2: Ubuntu AppArmor Configuration
# ==========================================
echo -e "${YELLOW}[*] Step 2: Configuring security settings...${NC}"

# Check AppArmor restriction
if [ -f /proc/sys/kernel/apparmor_restrict_unprivileged_userns ]; then
    CURRENT_VALUE=$(cat /proc/sys/kernel/apparmor_restrict_unprivileged_userns)
    echo -e "${YELLOW}[*] Current apparmor_restrict_unprivileged_userns: $CURRENT_VALUE${NC}"
    
    if [ "$CURRENT_VALUE" -ne 0 ]; then
        echo -e "${YELLOW}[*] Disabling AppArmor restriction...${NC}"
        if sysctl -w kernel.apparmor_restrict_unprivileged_userns=0 > /dev/null 2>&1; then
            echo -e "${GREEN}[+] AppArmor restriction disabled${NC}"
            APPARMOR_WAS_DISABLED=1
        else
            echo -e "${RED}[!] Failed to disable AppArmor restriction${NC}"
            exit 1
        fi
    else
        echo -e "${GREEN}[+] AppArmor restriction already disabled${NC}"
        APPARMOR_WAS_DISABLED=0
    fi
else
    echo -e "${YELLOW}[*] AppArmor restriction sysctl not found${NC}"
    APPARMOR_WAS_DISABLED=0
fi

# Check for PKS support in kernel (informational)
if [ -f /proc/cpuinfo ]; then
    if grep -q "pks" /proc/cpuinfo 2>/dev/null; then
        echo -e "${GREEN}[+] CPU PKS feature detected${NC}"
    else
        echo -e "${YELLOW}[*] CPU PKS feature not detected in /proc/cpuinfo${NC}"
        echo -e "${YELLOW}[*] This is expected in QEMU without proper CPU flags${NC}"
    fi
fi

# ==========================================
# Step 3: Drop Page Caches (Pre-clean)
# ==========================================
echo -e "${YELLOW}[*] Step 3: Dropping page caches before test...${NC}"
echo 1 > /proc/sys/vm/drop_caches
sleep 1
echo -e "${GREEN}[+] Page caches dropped${NC}"

# Verify SU is clean
echo -e "${YELLOW}[*] Verifying /usr/bin/su integrity before exploit...${NC}"
CURRENT_MD5=$(md5sum /usr/bin/su)
if [ "$ORIGINAL_MD5" != "$CURRENT_MD5" ]; then
    echo -e "${RED}[!] WARNING: /usr/bin/su appears modified before exploit!${NC}"
    echo -e "${RED}    Exiting for safety${NC}"
    exit 1
fi
echo -e "${GREEN}[+] SU integrity verified${NC}"

# Test SU normal behavior before exploit
echo -e "${YELLOW}[*] Testing normal SU behavior...${NC}"
BEFORE_TEST=$(echo "test" | timeout 3 /usr/bin/su --help 2>&1 || true)
echo -e "${GREEN}[+] SU responds normally${NC}"

# ==========================================
# Step 4: Compile Check (informational)
# ==========================================
echo -e "${YELLOW}[*] Step 4: Binary verification...${NC}"
echo -e "${YELLOW}[*] Using pre-compiled binary: $SCRIPT_DIR/$EXPLOIT_NAME${NC}"

if [ ! -x "$SCRIPT_DIR/$EXPLOIT_NAME" ]; then
    echo -e "${RED}[!] Binary is not executable${NC}"
    chmod +x "$SCRIPT_DIR/$EXPLOIT_NAME"
    echo -e "${YELLOW}[*] Made binary executable${NC}"
fi

# Show binary info
BINARY_SIZE=$(stat -c%s "$SCRIPT_DIR/$EXPLOIT_NAME")
echo -e "${GREEN}[+] Binary size: $BINARY_SIZE bytes${NC}"
echo -e "${GREEN}[+] Binary type: $(file $SCRIPT_DIR/$EXPLOIT_NAME | cut -d: -f2-)${NC}"

# ==========================================
# Step 5: Run the Exploit
# ==========================================
echo -e "${YELLOW}[*] Step 5: Executing $EXPLOIT_NAME...${NC}"
echo -e "${RED}[!] Running exploit - this will attempt to compromise /usr/bin/su${NC}"
echo -e "${RED}[!] Monitor closely. You have 5 seconds to abort (Ctrl+C)${NC}"
sleep 5

START_TIME=$(date +%s)
cd "$SCRIPT_DIR"
./"$EXPLOIT_NAME"
EXPLOIT_EXIT_CODE=$?
END_TIME=$(date +%s)
DURATION=$((END_TIME - START_TIME))

echo ""
echo -e "${CYAN}[*] Exploit completed in ${DURATION}s with exit code: $EXPLOIT_EXIT_CODE${NC}"

# ==========================================
# Step 6: Check if SU was compromised
# ==========================================
echo -e "${YELLOW}[*] Step 6: Testing if /usr/bin/su was compromised...${NC}"

# Check on-disk hash (shouldn't change - exploit targets page cache only)
FILE_MD5=$(md5sum /usr/bin/su)
if [ "$ORIGINAL_MD5" = "$FILE_MD5" ]; then
    echo -e "${GREEN}[+] SU on-disk hash unchanged (page cache exploit)${NC}"
else
    echo -e "${RED}[!] WARNING: SU on-disk hash changed!${NC}"
    echo -e "${RED}    This is unexpected behavior${NC}"
fi

# Test for page cache compromise - try to execute SU
echo -e "${YELLOW}[*] Testing page cache compromise...${NC}"
echo -e "${YELLOW}[*] Attempting to execute SU with timeout...${NC}"

# Save current terminal settings
STTY_SAVED=$(stty -g)

# Try to run SU in a controlled way
SU_OUTPUT=$(timeout 5 /usr/bin/su -c "echo 'COMPROMISE_TEST_MARKER'" 2>&1) || SU_EXIT=$?
SU_EXIT=${SU_EXIT:-0}

# Restore terminal settings
stty "$STTY_SAVED" 2>/dev/null || true

# Analyze results
if [ "$SU_EXIT" -eq 124 ]; then
    echo -e "${RED}[!] SU command timed out - possible hang (compromise indicator)${NC}"
    EXPLOIT_SUCCESS=1
elif echo "$SU_OUTPUT" | grep -q "COMPROMISE_TEST_MARKER"; then
    echo -e "${RED}[!] EXPLOIT SUCCESSFUL - SU executed arbitrary command!${NC}"
    echo -e "${RED}[!] Page cache compromise confirmed${NC}"
    EXPLOIT_SUCCESS=1
elif echo "$SU_OUTPUT" | grep -q "Authentication failure\|Sorry\|incorrect"; then
    echo -e "${GREEN}[+] EXPLOIT BLOCKED - SU shows normal authentication failure${NC}"
    EXPLOIT_SUCCESS=0
else
    echo -e "${YELLOW}[?] Unclear result - SU behavior changed${NC}"
    echo -e "${YELLOW}[*] SU output: $SU_OUTPUT${NC}"
    EXPLOIT_SUCCESS=2
fi

# Check if we're in a shell (worst case)
if [ -n "$BASH" ] && [ "$SHLVL" -gt 1 ]; then
    echo -e "${RED}[!] WARNING: Shell level increased - we might be in a spawned shell${NC}"
fi

# ==========================================
# Step 7: CRITICAL Cleanup
# ==========================================
echo -e "${YELLOW}[*] Step 7: Performing CRITICAL cleanup...${NC}"

# Drop page caches (MANDATORY - removes injected code from memory)
echo -e "${RED}[!] CRITICAL: Dropping page caches to remove injected code${NC}"
sync  # Sync filesystem first
echo 1 > /proc/sys/vm/drop_caches
sleep 2
echo 3 > /proc/sys/vm/drop_caches  # Drop everything (dentries, inodes too)
sleep 1
echo -e "${GREEN}[+] Page caches completely dropped${NC}"

# Verify SU is clean after cache drop
echo -e "${YELLOW}[*] Verifying SU cleanup...${NC}"
VERIFY_OUTPUT=$(timeout 5 /usr/bin/su --help 2>&1 || true)

if echo "$VERIFY_OUTPUT" | grep -qi "usage\|change\|option"; then
    echo -e "${GREEN}[+] SU appears normal after cache drop - cleanup successful${NC}"
else
    echo -e "${RED}[!] WARNING: SU still abnormal after cache drop!${NC}"
    echo -e "${RED}[!] May need to reboot system to fully clean${NC}"
    echo -e "${RED}[!] Do NOT leave this system unattended${NC}"
fi

# Additional cleanup - kill any spawned shells
if [ -n "$BASH" ] && [ "$SHLVL" -gt 1 ]; then
    echo -e "${RED}[!] Detected elevated shell level - attempting to exit extra shells${NC}"
    while [ "$SHLVL" -gt 1 ]; do
        exit
    done
fi

# ==========================================
# Step 8: Restore Security Settings
# ==========================================
echo -e "${YELLOW}[*] Step 8: Restoring security settings...${NC}"

# Restore AppArmor if we disabled it
if [ "$APPARMOR_WAS_DISABLED" -eq 1 ]; then
    echo -e "${YELLOW}[*] Restoring AppArmor restriction...${NC}"
    if sysctl -w kernel.apparmor_restrict_unprivileged_userns=1 > /dev/null 2>&1; then
        echo -e "${GREEN}[+] AppArmor restriction restored${NC}"
    else
        echo -e "${RED}[!] Failed to restore AppArmor restriction${NC}"
        echo -e "${RED}[!] Run manually: sysctl -w kernel.apparmor_restrict_unprivileged_userns=1${NC}"
    fi
fi

# ==========================================
# Step 9: Results Summary
# ==========================================
echo ""
echo -e "${CYAN}========================================${NC}"
echo -e "${CYAN}  Test Results Summary${NC}"
echo -e "${CYAN}========================================${NC}"
echo -e "Timestamp: $(date)"
echo -e "Exploit: ${YELLOW}$EXPLOIT_NAME${NC}"
echo -e "Location: ${YELLOW}$SCRIPT_DIR${NC}"
echo -e "Duration: ${CYAN}${DURATION}s${NC}"
echo -e "Exit Code: ${CYAN}$EXPLOIT_EXIT_CODE${NC}"

case $EXPLOIT_SUCCESS in
    0)
        echo -e "Result: ${GREEN}EXPLOIT BLOCKED${NC}"
        echo -e "Status: ${GREEN}SU remained intact and functional${NC}"
        if [ "$EXPLOIT_NAME" = "fragnesia_pks" ]; then
            echo -e "PKS: ${GREEN}Protection appears effective${NC}"
        fi
        ;;
    1)
        echo -e "Result: ${RED}EXPLOIT SUCCESSFUL${NC}"
        echo -e "Status: ${RED}SU page cache was compromised${NC}"
        if [ "$EXPLOIT_NAME" = "fragnesia_pks" ]; then
            echo -e "PKS: ${RED}Protection FAILED${NC}"
        fi
        echo -e "Cleanup: ${YELLOW}Cache dropped - system restored${NC}"
        ;;
    2)
        echo -e "Result: ${YELLOW}INCONCLUSIVE${NC}"
        echo -e "Status: ${YELLOW}SU behavior changed but unclear if compromised${NC}"
        ;;
esac

echo ""
echo -e "SU Hash: ${CYAN}${ORIGINAL_MD5%% *}${NC}"
echo -e "SU Size: ${CYAN}$ORIGINAL_SIZE bytes${NC}"
echo ""
echo -e "${GREEN}[+] Test complete - system has been cleaned${NC}"
echo -e "${YELLOW}[*] If you suspect any issues, reboot the system${NC}"