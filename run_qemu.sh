#!/bin/bash
set -e

# ==========================================
# Self‑contained test environment
# ==========================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EXPLOIT_SRC="$SCRIPT_DIR/_fragnesia"
KERNEL_IMAGE="$SCRIPT_DIR/build/arch/x86/boot/bzImage"

SANDBOX_DIR="/tmp/qemu-sandbox-$$"
INITRAMFS="/tmp/initramfs-$$.cpio"

# --------------------------------------------------
# Pre‑flight checks
# --------------------------------------------------
[ -f "$KERNEL_IMAGE" ] || { echo "Error: Kernel not built? $KERNEL_IMAGE"; exit 1; }
[ -d "$EXPLOIT_SRC" ]   || { echo "Error: $EXPLOIT_SRC missing"; exit 1; }

QEMU=$(which qemu-system-x86_64 2>/dev/null) || {
    echo "Error: qemu-system-x86_64 not found. Install qemu-system-x86."
    exit 1
}

command -v busybox &>/dev/null || {
    echo "Installing busybox-static..."
    sudo apt update && sudo apt install -y busybox-static
}
BUSYBOX_PATH=$(which busybox)

# --------------------------------------------------
# Compile exploits
# --------------------------------------------------
echo "Compiling exploits in $EXPLOIT_SRC ..."
cd "$EXPLOIT_SRC"
for src in fragnesia.c fragnesia_pks.c; do
    if [ -f "$src" ]; then
        out="${src%.c}"
        echo "  -> $out"
        gcc -O2 -Wall -static -o "$out" "$src" -lpthread 2>/dev/null || \
        gcc -O2 -Wall -static -o "$out" "$src" 2>/dev/null
    fi
done
[ -f run.sh ] && chmod +x run.sh
cd "$SCRIPT_DIR"

# --------------------------------------------------
# Build initramfs
# --------------------------------------------------
echo "Creating initramfs..."
rm -rf "$SANDBOX_DIR"
mkdir -p "$SANDBOX_DIR"/{bin,proc,sys,dev,exploit,etc,home/testuser,usr/bin}

# BusyBox + symlinks
cp "$BUSYBOX_PATH" "$SANDBOX_DIR/bin/busybox"
(
    cd "$SANDBOX_DIR/bin"
    for cmd in $(./busybox --list); do
        ln -s busybox "$cmd" 2>/dev/null || true
    done
)

# No need for external ip – BusyBox's ifconfig handles loopback

# Exploit files
cp "$EXPLOIT_SRC"/fragnesia      "$SANDBOX_DIR/exploit/" 2>/dev/null || true
cp "$EXPLOIT_SRC"/fragnesia_pks  "$SANDBOX_DIR/exploit/" 2>/dev/null || true
cp "$EXPLOIT_SRC"/run.sh         "$SANDBOX_DIR/exploit/"
chmod +x "$SANDBOX_DIR/exploit/"*

# --------------------------------------------------
# Init script – simple and robust
# --------------------------------------------------
cat > "$SANDBOX_DIR/init" << 'INITEOF'
#!/bin/busybox sh

mount -t proc proc /proc
mount -t sysfs sys /sys
mount -t devtmpfs dev /dev

# Loopback
ifconfig lo up 2>/dev/null || true

# Allow unprivileged user namespaces (both sysctl variants)
echo 0 > /proc/sys/kernel/apparmor_restrict_unprivileged_userns 2>/dev/null || true
echo 1 > /proc/sys/kernel/unprivileged_userns_clone 2>/dev/null || true

# Create testuser (non‑root)
mkdir -p /etc /home/testuser
echo "root:x:0:0:root:/root:/bin/sh"          > /etc/passwd
echo "testuser:x:1000:1000::/home/testuser:/bin/sh" >> /etc/passwd
echo "root:x:0:"                              > /etc/group
echo "testuser:x:1000:"                      >> /etc/group
echo "root::0:0:99999:7:::"                  > /etc/shadow
echo "testuser::0:0:99999:7:::"             >> /etc/shadow
chown -R 1000:1000 /home/testuser 2>/dev/null || true

# Target: regular file, 755, not writable by testuser
cp /bin/busybox /usr/bin/su
chown root:root /usr/bin/su
chmod 755 /usr/bin/su

chmod -R 777 /exploit

echo "================================================="
echo " PKS Fragnesia Sandbox Ready"
echo "================================================="
echo "Logged in as 'testuser' (non-root)"
echo "Exploits in /exploit"
echo ""

# Persistent shell loop – drop caches on each restart to clear any page-cache injection
while true; do
    # CRITICAL: evict any injected page-cache data from previous runs
    sync
    echo 3 > /proc/sys/vm/drop_caches 2>/dev/null || true

    setsid cttyhack su - testuser
    echo "Shell closed. Restarting..."
    sleep 1
done
INITEOF

chmod +x "$SANDBOX_DIR/init"

# --------------------------------------------------
# Package & launch
# --------------------------------------------------
echo "Packaging initramfs..."
cd "$SANDBOX_DIR"
find . | cpio -o -H newc > "$INITRAMFS" 2>/dev/null
cd "$SCRIPT_DIR"

echo "Starting QEMU..."
$QEMU \
    -kernel "$KERNEL_IMAGE" \
    -initrd "$INITRAMFS" \
    -cpu max,pks=on \
    -smp 1 \
    -m 2G \
    -nographic \
    -append "console=ttyS0 nokaslr" \
    -no-reboot

rm -rf "$SANDBOX_DIR" "$INITRAMFS"