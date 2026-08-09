#include <sys/syscall.h>
#include <fcntl.h>
#include <unistd.h>

/* Replace with the actual syscall numbers you used in the kernel */
#ifndef SYS_pks_file_set
#define SYS_pks_file_set 472
#define SYS_pks_set      473
#endif

int pks_protection() {
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
}