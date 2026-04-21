.set PAGE_PRESENT, (1 << 0)
.set PAGE_WRITE, (1 << 1)
.set PAGE_HUGE, (1 << 7)

.section .text
.global boot_64
.extern kernel_main
.code32
boot_64:
    # first, check CPU support long mode
    movl $0x80000000, %eax
    cpuid
    cmpl $0x80000001, %eax
    jb no_long_mode

    movl $0x80000001, %eax
    cpuid
    testl $(1 << 29), %edx
    jz no_long_mode

    # second, set page table
    # clean PML4
    movl $pml4, %edi
    xorl %eax, %eax
    movl $1024, %ecx
    rep stosl

    # set PML4[0] point to PDP
    movl $pml4, %edi
    movl $pdp, %eax
    orl $(PAGE_PRESENT | PAGE_WRITE), %eax
    movl %eax, (%edi)

    # set PDP[0] point to PD
    movl $pd, %eax
    orl $(PAGE_PRESENT | PAGE_WRITE), %eax
    movl $pdp, %edi
    movl %eax, (%edi)

    # set PD table, use 2MB huge page mode to map
    # first 1 GB memory
    movl $pd, %edi
    movl $0, %eax   # from physical address 0
    movl $512, %ecx     # 512 entrys

1:
    movl %eax, (%edi)
    orl $(PAGE_PRESENT | PAGE_WRITE | PAGE_HUGE), (%edi)
    addl $0x200000, %eax    # per page 2MB
    addl $8, %edi   # per entry 8 bytes
    loop 1b

    # third, use PAE
    movl %cr4, %eax
    orl $(1 << 5), %eax     # CR4.PAE = 1
    movl %eax, %cr4

    # fourth, load PML4 address to CR3
    movl $pml4, %eax
    movl %eax, %cr3

    # fifth, use long mode (set EFER.LME)
    movl $0xC0000080, %ecx  # EFER MSR num
    rdmsr
    orl $(1 << 8), %eax     # set LME bit
    wrmsr

    # sixth, use paging to active long mode
    movl %cr0, %eax
    orl $(1 << 31), %eax    # CR0.PG = 1
    movl %eax, %cr0

    # seventh， load 64 bit GDT and jmp to 64 bit code
    lgdt gdt64_ptr

    # far jmp to enter 64 bit code section
    ljmp $0x08, $start64

.code32
no_long_mode:
    movw $0xB800, %ax
    movw %ax, %es
    movw $'N', %es:0
    movw $0x07, %es:1
    movw $'O', %es:2
    movw $0x07, %es:3
    movw $' ', %es:4
    movw $0x07, %es:5
    movw $'6', %es:6
    movw $0x07, %es:7
    movw $'4', %es:8
    movw $0x07, %es:9

    cli
1:  hlt
    jmp 1b

.code64
start64:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss

    # setup stack
    movq $0x90000, %rsp

    # debug: print "64-bit mode OK!" to VGA
    movq $0xB8000, %rdi
    movb $'6', (%rdi)
    movb $0x0A, 1(%rdi)
    movb $'4', 2(%rdi)
    movb $0x0A, 3(%rdi)
    movb $'-', 4(%rdi)
    movb $0x0A, 5(%rdi)
    movb $'b', 6(%rdi)
    movb $0x0A, 7(%rdi)
    movb $'i', 8(%rdi)
    movb $0x0A, 9(%rdi)
    movb $'t', 10(%rdi)
    movb $0x0A, 11(%rdi)
    movb $' ', 12(%rdi)
    movb $0x0A, 13(%rdi)
    movb $'O', 14(%rdi)
    movb $0x0A, 15(%rdi)
    movb $'K', 16(%rdi)
    movb $0x0A, 17(%rdi)
    movb $'!', 18(%rdi)
    movb $0x0A, 19(%rdi)

    # now in 64 bit mode, call C kernel
    call kernel_main

    cli
1:  hlt
    jmp 1b
    
.section .data
.align 4096
# page table structure
pml4:
    .skip 4096
pdp:
    .skip 4096
pd:
    .skip 4096

# 64 bit GDT
.align 8
gdt64:
    .quad 0x0000000000000000    # empty descriptor
    .quad 0x0020980000000000    # code segement descriptor (execute/read, 64bit)
    .quad 0x0000920000000000    # data segement descriptor (read/write)
gdt64_end:

# GDT pointer
gdt64_ptr:
    .word gdt64_end - gdt64 - 1     # limit
    .quad gdt64     # base
