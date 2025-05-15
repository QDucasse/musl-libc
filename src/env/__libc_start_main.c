#include <elf.h>
#include <poll.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include "syscall.h"
#include <sys/mman.h>
#include "atomic.h"
#include "libc.h"

static void dummy(void) {}
weak_alias(dummy, _init);

extern weak hidden void (*const __init_array_start)(void), (*const __init_array_end)(void);

static void dummy1(void *p) {}
weak_alias(dummy1, __init_ssp);

#define AUX_CNT 38

#ifdef __GNUC__
__attribute__((__noinline__))
#endif
void __init_libc(char **envp, char *pn)
{
	size_t i, *auxv, aux[AUX_CNT] = { 0 };
	__environ = envp;
	for (i=0; envp[i]; i++);
	libc.auxv = auxv = (void *)(envp+i+1);
	for (i=0; auxv[i]; i+=2) if (auxv[i]<AUX_CNT) aux[auxv[i]] = auxv[i+1];
	__hwcap = aux[AT_HWCAP];
	if (aux[AT_SYSINFO]) __sysinfo = aux[AT_SYSINFO];
	libc.page_size = aux[AT_PAGESZ];

	if (!pn) pn = (void*)aux[AT_EXECFN];
	if (!pn) pn = "";
	__progname = __progname_full = pn;
	for (i=0; pn[i]; i++) if (pn[i]=='/') __progname = pn+i+1;

	__init_tls(aux);
	__init_ssp((void *)aux[AT_RANDOM]);

	if (aux[AT_UID]==aux[AT_EUID] && aux[AT_GID]==aux[AT_EGID]
		&& !aux[AT_SECURE]) return;

	struct pollfd pfd[3] = { {.fd=0}, {.fd=1}, {.fd=2} };
	int r =
#ifdef SYS_poll
	__syscall(SYS_poll, pfd, 3, 0);
#else
	__syscall(SYS_ppoll, pfd, 3, &(struct timespec){0}, 0, _NSIG/8);
#endif
	if (r<0) a_crash();
	for (i=0; i<3; i++) if (pfd[i].revents&POLLNVAL)
		if (__sys_open("/dev/null", O_RDWR)<0)
			a_crash();
	libc.secure = 1;
}

static void libc_start_init(void)
{
	_init();
	uintptr_t a = (uintptr_t)&__init_array_start;
	for (; a<(uintptr_t)&__init_array_end; a+=sizeof(void(*)()))
		(*(void (**)(void))a)();
}


/* ────────────────────────── HBNG - STM SETUP ─────────────────────────────── */

#define STM_STIMULUS_BASE 0xF8000000UL
#define STM_MAP_SIZE 0x1000

static void* stm_region_ptr = NULL;

/**
 * Preload the STM region, opening the file descriptor, mapping it in memory
 * and storing the obtained address in a global variable (FIXME: in TLS)
 */
void preload_stm_region(void)
{
  /* Open /dev/mem to access the STM address */
  int fd = open("/dev/mem", O_RDWR | O_SYNC);
  if (fd < 0) {
    return;
  }

  /* Map the STM region in userspace */
  void* mapped_region = mmap(NULL, STM_MAP_SIZE, PROT_READ | PROT_WRITE,
                             MAP_SHARED, fd, STM_STIMULUS_BASE);
  if (mapped_region == MAP_FAILED) {
    close(fd);
    mapped_region = NULL;
    return;
  }

  /* Close the /dev/mem file descriptor */
  close(fd);

  /* Store the virtual address in the TLS */
  stm_region_ptr = mapped_region;

  /* Load the TLS pointer into x28 */
  __asm__ __volatile__("mov x28, %0" ::"r"(stm_region_ptr));
}

/**
 * Unmaps the STM region from userspace
 */
void unload_stm_region(void)
{
  if (stm_region_ptr != NULL) {
    munmap(stm_region_ptr, STM_MAP_SIZE);
  }
}

/* ──────────────────────────────────────────────────────────────────────── */

weak_alias(libc_start_init, __libc_start_init);

typedef int lsm2_fn(int (*)(int,char **,char **), int, char **);
static lsm2_fn libc_start_main_stage2;

int __libc_start_main(int (*main)(int,char **,char **), int argc, char **argv,
	void (*init_dummy)(), void(*fini_dummy)(), void(*ldso_dummy)())
{
	char **envp = argv+argc+1;

	/* External linkage, and explicit noinline attribute if available,
	 * are used to prevent the stack frame used during init from
	 * persisting for the entire process lifetime. */
	__init_libc(envp, argv[0]);

	/* HBNG - STM SETUP*/
	preload_stm_region();

	/* Barrier against hoisting application code or anything using ssp
	 * or thread pointer prior to its initialization above. */
	lsm2_fn *stage2 = libc_start_main_stage2;
	__asm__ ( "" : "+r"(stage2) : : "memory" );
	return stage2(main, argc, argv);
}

static int libc_start_main_stage2(int (*main)(int,char **,char **), int argc, char **argv)
{
	char **envp = argv+argc+1;
	__libc_start_init();

	/* ──────── HBNG - SP INSTRUMENTATION HOOK ──────── */
	{
		/* Capture the stack pointer */
		__asm__ __volatile__("mov x27, sp");
		__asm__ __volatile__("str x27, [x28]");
	}
	/* ────────────────------───────────────────────── */

	/* Pass control to the application */
	exit(main(argc, argv, envp));

	/* HBNG - STM teardown */
	unload_stm_region();

	return 0;
}
