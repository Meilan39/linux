Here are the exact steps you need to follow to catch the Fragnesia exploit using your Intel PKS implementation:

### 1. Update the Kernel Page Fault Handler

Currently, when the PKS blocks a write access in kernel mode (which is what the exploit does via the ESP-in-TCP decryption loop), the kernel will treat it as an unhandled kernel page fault and crash. We need to intercept this and gracefully terminate the process instead.

Open `arch/x86/mm/fault.c` and locate the `do_kern_addr_fault()` function. Add a check for the `X86_PF_PK` (Protection Key) error code before the oops logic:

```c
static void
do_kern_addr_fault(struct pt_regs *regs, unsigned long hw_error_code,
		   unsigned long address)
{
	/* ... existing code ... */

	/* ADD THIS: Catch PKS violations and terminate the exploit */
	if (unlikely(hw_error_code & X86_PF_PK)) {
		pr_err("PKS write-protection violation caught at %p!\n", (void *)address);
		if (!in_interrupt()) {
			do_exit(SIGKILL); // Gracefully kill the process instead of Oopsing
		}
	}

	/* ... existing code (e.g., bad_area_nosemaphore, kernelmode_fixup_or_oops) ... */
}
```

After applying this change, recompile and install your kernel:
```bash
make -j$(nproc)
sudo make modules_install
sudo make install
```
*(Make sure to reboot into the newly compiled kernel before proceeding to step 2).*

### 2. Modify the Exploit (`fragnesia.c`)

Next, we need to instruct the exploit to apply your PKS protection to `/usr/bin/su` right before it attempts to run the ESP-in-TCP trigger. 

Open `pocs/fragnesia/fragnesia.c` and modify the `main` function. Make sure to define your custom syscall numbers at the top of the file if they aren't included in your userspace headers.

```c
#include <sys/syscall.h>
#include <fcntl.h>
#include <unistd.h>

/* Replace with the actual syscall numbers you used in the kernel */
#ifndef SYS_pks_file_set
#define SYS_pks_file_set 451
#define SYS_pks_set      452
#endif

int main(int argc, char **argv)
{
	/* ... existing code ... */

	file_size = use_existing_target("/usr/bin/su");

	/* --- ADDED PKS MITIGATION --- */
	printf("[*] Setting up PKS protection for /usr/bin/su\n");
	int pks_fd = open("/usr/bin/su", O_RDONLY);
	if (pks_fd < 0) {
		perror("open target for PKS");
		return 1;
	}
	
	// Assign /usr/bin/su to Key 1
	if (syscall(SYS_pks_file_set, pks_fd, 1) != 0) {
		perror("SYS_pks_file_set");
	}
	
	// Set PKS permissions for Key 1: AD=0 (Access allow), WD=1 (Write disable)
	if (syscall(SYS_pks_set, 1, 0, 1) != 0) {
		perror("SYS_pks_set");
	}
	close(pks_fd);
	/* ---------------------------- */

	byte_off = 0;
	
	/* ... existing code ... */
}
```
*Note: We open `/usr/bin/su` with `O_RDONLY` because the exploit naturally only has read access. Your `pks_file_set` syscall will apply the protection key to the underlying page cache folios, catching the kernel when it unlawfully attempts to write there.*

### 3. Compile and Test

Compile the modified exploit and run it. The exploit will now fail gracefully and your kernel will stay alive!

```bash
cd pocs/fragnesia
gcc -o exp fragnesia.c
./exp
```

When you run `./exp`, the exploit should trigger the `do_exit(SIGKILL)` inside `do_kern_addr_fault`, killing the receiver process when it attempts the splice write. You will see the trigger pair fail and if you check `dmesg`, you should see your `PKS write-protection violation caught` log.