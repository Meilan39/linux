#!/bin/bash
set -e

# ==========================================
# Configuration
# ==========================================
KERNEL_IMAGE="$HOME/src/linux-fragnesia-test/build/arch/x86/boot/bzImage"
SANDBOX_DIR="$HOME/qemu-sandbox"
INITRAMFS="$HOME/initramfs.cpio"
EXPLOIT_SRC="$HOME/src/linux-fragnesia-test/_fragnesia"
EXPLOIT_DIR="$HOME/exploit_src"

# ==========================================
# Pre-flight Checks
# ==========================================
if [ ! -f "$KERNEL_IMAGE" ]; then
    echo "Error: Kernel image not found at $KERNEL_IMAGE"
    exit 1
fi

if [ ! -d "$EXPLOIT_SRC" ]; then
    echo "Error: Exploit source directory not found at $EXPLOIT_SRC"
    echo "Please create it and place fragnesia.c, fragnesia_pks.c, pks.h, and run.sh there"
    exit 1
fi

# ==========================================
# Compile Exploits on Host
# ==========================================
echo "Compiling exploits on host..."
mkdir -p "$EXPLOIT_DIR"
cd "$EXPLOIT_SRC"

# Compile unprotected version
if [ -f "fragnesia.c" ]; then
    echo "  Compiling fragnesia (unprotected)..."
    gcc -O2 -Wall -static -o "$EXPLOIT_DIR/fragnesia" fragnesia.c -lpthread
    echo "  Done"
fi

# Compile PKS-protected version
if [ -f "fragnesia_pks.c" ] && [ -f "pks.h" ]; then
    echo "  Compiling fragnesia_pks (PKS protected)..."
    gcc -O2 -Wall -static -o "$EXPLOIT_DIR/fragnesia_pks" fragnesia_pks.c -lpthread
    echo "  Done"
fi

# Copy run.sh to exploit directory
if [ -f "run.sh" ]; then
    cp run.sh "$EXPLOIT_DIR/"
    chmod +x "$EXPLOIT_DIR/run.sh"
fi

# ==========================================
# Build the Minimal Filesystem
# ==========================================
echo "Setting up the initramfs in $SANDBOX_DIR..."
rm -rf "$SANDBOX_DIR"
mkdir -p "$SANDBOX_DIR/bin" "$SANDBOX_DIR/proc" "$SANDBOX_DIR/sys" "$SANDBOX_DIR/dev" "$SANDBOX_DIR/exploit"

# Install Busybox
cp /bin/busybox "$SANDBOX_DIR/bin/"
"$SANDBOX_DIR/bin/busybox" --install -s "$SANDBOX_DIR/bin"

# Function to safely copy dynamic binaries
install_dynamic_bin() {
    local BIN_PATH=$(which $1 2>/dev/null)
    if [ -z "$BIN_PATH" ]; then
        echo "Warning: Command '$1' not found - skipping"
        return
    fi
    
    echo "  Installing $1..."
    cp "$BIN_PATH" "$SANDBOX_DIR/bin/"
    
    ldd "$BIN_PATH" 2>/dev/null | awk '
        /=>/ { if ($3 ~ /^\//) print $3 }
        /^\t\// { print $1 }
    ' | while read -r lib; do
        mkdir -p "$SANDBOX_DIR$(dirname "$lib")"
        cp -L "$lib" "$SANDBOX_DIR$lib" 2>/dev/null || true
    done
}

# Install required tools
echo "Installing system tools..."
install_dynamic_bin "ip"
install_dynamic_bin "su"
install_dynamic_bin "md5sum"
install_dynamic_bin "sha256sum"

# Copy compiled exploits and scripts
echo "Copying exploits to sandbox..."
cp "$EXPLOIT_DIR/fragnesia" "$SANDBOX_DIR/exploit/" 2>/dev/null || true
cp "$EXPLOIT_DIR/fragnesia_pks" "$SANDBOX_DIR/exploit/" 2>/dev/null || true
cp "$EXPLOIT_DIR/run.sh" "$SANDBOX_DIR/exploit/" 2>/dev/null || true
chmod +x "$SANDBOX_DIR/exploit/"*

# Create the Boot Script
cat << 'EOF' > "$SANDBOX_DIR/init"
#!/bin/busybox sh

# Mount essential filesystems
mount -t proc proc /proc
mount -t sysfs sys /sys
mount -t devtmpfs dev /dev

# Bring up loopback interface (required for local TCP exploits)
/bin/ip link set lo up

# Configure AppArmor for Ubuntu compatibility
if [ -f /proc/sys/kernel/apparmor_restrict_unprivileged_userns ]; then
    echo 0 > /proc/sys/kernel/apparmor_restrict_unprivileged_userns
    echo "AppArmor: Unprivileged user namespaces enabled"
fi

echo "================================================="
echo " PKS Fragnesia Sandbox Ready"
echo "================================================="
echo "Exploits:"
ls -la /exploit/ 2>/dev/null
echo ""
echo "Commands:"
echo "  /exploit/run.sh fragnesia      - Run unprotected"
echo "  /exploit/run.sh fragnesia_pks  - Run PKS protected"
echo ""
echo "Manual test:"
echo "  /exploit/fragnesia             - Unprotected"
echo "  /exploit/fragnesia_pks         - PKS protected"
echo "================================================="

# Drop to shell
exec /bin/sh
EOF

chmod +x "$SANDBOX_DIR/init"

# ==========================================
# Package and Launch
# ==========================================
echo "Packaging initramfs..."
cd "$SANDBOX_DIR"
find . | cpio -o -H newc > "$INITRAMFS" 2>/dev/null
cd "$HOME"

echo "Starting QEMU with PKS support..."
echo "  Kernel: $KERNEL_IMAGE"
echo "  Initramfs: $INITRAMFS"
echo ""

qemu-system-x86_64 \
    -kernel "$KERNEL_IMAGE" \
    -initrd "$INITRAMFS" \
    -cpu max,pks=on \
    -m 2G \
    -nographic \
    -append "console=ttyS0 nokaslr" \
    -no-reboot