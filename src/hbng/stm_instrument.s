    .section .bss
    .global stm_region_ptr
stm_region_ptr:
    .quad 0

// preload_stm_region.S
    .section .text
    .global preload_stm_region
    .type preload_stm_region, %function

preload_stm_region:
    // openat syscall
    mov x0, -100         // AT_FDCWD
    ldr x1, =path_str    // pointer to "/dev/mem"
    ldr x2, =O_RDWR_SYNC
    mov x8, SYS_openat
    svc 0
    cmp x0, #0
    blt done             // if fd < 0, return
    mov x19, x0          // save fd in callee saved register

    // mmap syscall
    mov x0, 0            // addr = NULL
    mov x1, STM_MAP_SIZE
    mov x2, PROT_RW
    mov x3, MAP_SHARED
    mov x4, x19          // fd
    ldr x5, =STM_STIMULUS_BASE
    mov x8, SYS_mmap
    svc 0
    cmp x0, #-4095       // check error in range
    bhi mmap_fail
    mov x20, x0          // save mapped address

    // close syscall
    mov x0, x19          // fd
    mov x8, SYS_close
    svc 0

    // set global pointer (use symbol)
    ldr x1, =stm_region_ptr
    str x20, [x1]

    // move to x26 and store sp to [x26]
    mov x26, x20
    mov x25, sp
    str x25, [x26]

done:
    ret

mmap_fail:
    // handle error cleanup if needed
    mov x0, x19
    mov x8, SYS_close
    svc 0
    b done

    .section .rodata
path_str:
    .asciz "/dev/mem"

    .equ O_RDWR_SYNC, 0x0002 | 0x101000
    .equ PROT_RW, 0x1 | 0x2
    .equ MAP_SHARED, 0x01
    .equ SYS_openat, 56
    .equ SYS_mmap, 222
    .equ SYS_close, 57
    .equ STM_STIMULUS_BASE, 0xF8000000
    .equ STM_MAP_SIZE, 0x1000