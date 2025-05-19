## STMusl: custom libc for CoreSight STM setup

### Description

This customized version of the `musl-libc` adds a routine to map the CoreSight STM module in user memory and writes the value of the stack pointer before launching the user program.

The main changes are located in `src/env/__libc_start_main.c` and contain **(1)** the STM mapping and unmapping, **(2)** the stack pointer writing to STM.

**STM mapping and unmapping**
```c
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

  /* Load the TLS pointer into x26 */
  __asm__ __volatile__("mov x26, %0" ::"r"(stm_region_ptr));
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
```

*Modifications:*
```diff
int __libc_start_main(int (*main)(int,char **,char **), int argc, char **argv,
	void (*init_dummy)(), void(*fini_dummy)(), void(*ldso_dummy)())
{
	char **envp = argv+argc+1;

	/* External linkage, and explicit noinline attribute if available,
	 * are used to prevent the stack frame used during init from
	 * persisting for the entire process lifetime. */
	__init_libc(envp, argv[0]);

+	/* HBNG - STM SETUP*/
+	preload_stm_region();

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

+	/* ──────── HBNG - SP INSTRUMENTATION HOOK ──────── */
+	{
+		/* Capture the stack pointer */
+		__asm__ __volatile__("mov x25, sp");
+		__asm__ __volatile__("str x25, [x26]");
+	}
+	/* ────────────────------───────────────────────── */

	/* Pass control to the application */
	exit(main(argc, argv, envp));

+	/* HBNG - STM teardown */
+	unload_stm_region();

	return 0;
}

```

### Build

The configure installs a directory that looks like:

```bash
<clone>
cd musl-libc
export CC=path/to/compiler  # If a custom compiler is used
export MUSL_ROOT=/opt/musl
./configure --prefix=$MUSL_ROOT/sysroot \
              --exec-prefix=$MUSL_ROOT/bin \
              --syslibdir=$MUSL_ROOT/sysroot/lib
make -j$(nproc)
make install
```

Compiling against the custom libc with llvm boils down to:

```bash
$BUILD_DIR/bin/clang \
  -static \
  -nostdlib \
  -fuse-ld=$BUILD_DIR/bin/ld.lld \
  -isystem $MUSL_ROOT$/musl/sysroot/include \
  $MUSL_ROOT/sysroot/lib/crt1.o \
  $MUSL_ROOT/sysroot/lib/crti.o \
  -o fib fib.c \
  -L$MUSL_ROOT/sysroot/lib -lc -lm \
  $MUSL_ROOT/sysroot/lib/crtn.o
```

---


> Fork of the [musl libc](https://git.musl-libc.org/cgit/musl/), commit `c47ad25ea3b484e10326f933e927c0bc8cded3da`

Original `README`:

```
musl libc

musl, pronounced like the word "mussel", is an MIT-licensed
implementation of the standard C library targetting the Linux syscall
API, suitable for use in a wide range of deployment environments. musl
offers efficient static and dynamic linking support, lightweight code
and low runtime overhead, strong fail-safe guarantees under correct
usage, and correctness in the sense of standards conformance and
safety. musl is built on the principle that these goals are best
achieved through simple code that is easy to understand and maintain.

The 1.1 release series for musl features coverage for all interfaces
defined in ISO C99 and POSIX 2008 base, along with a number of
non-standardized interfaces for compatibility with Linux, BSD, and
glibc functionality.

For basic installation instructions, see the included INSTALL file.
Information on full musl-targeted compiler toolchains, system
bootstrapping, and Linux distributions built on musl can be found on
the project website:

    http://www.musl-libc.org/
```