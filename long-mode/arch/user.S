.section .text

.global user_program
user_program:
    movq    $0xB8000, %rdi
    movb    $'h', (%rdi)
    movb    $0x0F, 1(%rdi)
    movb    $'e', 2(%rdi)
    movb    $0x0F, 3(%rdi)
    movb    $'l', 4(%rdi)
    movb    $0x0F, 5(%rdi)
    movb    $'l', 6(%rdi)
    movb    $0x0F, 7(%rdi)
    movb    $'o', 8(%rdi)
    movb    $0x0F, 9(%rdi)
    movb    $',', 10(%rdi)
    movb    $0x0F, 11(%rdi)
    movb    $' ', 12(%rdi)
    movb    $0x0F, 13(%rdi)
    movb    $'w', 14(%rdi)
    movb    $0x0F, 15(%rdi)
    movb    $'o', 16(%rdi)
    movb    $0x0F, 17(%rdi)
    movb    $'r', 18(%rdi)
    movb    $0x0F, 19(%rdi)
    movb    $'l', 20(%rdi)
    movb    $0x0F, 21(%rdi)
    movb    $'d', 22(%rdi)
    movb    $0x0F, 23(%rdi)

1:  jmp     1b

/* syscall_user_program — position-independent ring 3 code.
 * copied to user memory (0x300000) at runtime.
 *
 * demo: output a message, then loop reading a key and echoing it. */
.global syscall_user_program
syscall_user_program:
    /* output hello */
    movq    $1, %rax            /* SYS_OUTPUT */
    movq    $0, %rdi            /* dd = stdout */
    leaq    msg_hello(%rip), %rsi
    movq    $msg_hello_len, %rdx
    int     $0x80

1:
    /* output prompt */
    movq    $1, %rax
    movq    $0, %rdi
    leaq    msg_prompt(%rip), %rsi
    movq    $msg_prompt_len, %rdx
    int     $0x80

    /* input 1 char from keyboard into the stack buffer */
    movq    $2, %rax            /* SYS_INPUT */
    movq    $1, %rdi            /* dd = keyboard */
    leaq    buf(%rip), %rsi     /* buf (in .text, user-accessible) */
    movq    $1, %rdx            /* len = 1 */
    int     $0x80

    /* output "you typed: " */
    movq    $1, %rax
    movq    $0, %rdi
    leaq    msg_echo(%rip), %rsi
    movq    $msg_echo_len, %rdx
    int     $0x80

    /* output the char we just read */
    movq    $1, %rax
    movq    $0, %rdi
    leaq    buf(%rip), %rsi
    movq    $1, %rdx
    int     $0x80

    /* output newline */
    movq    $1, %rax
    movq    $0, %rdi
    leaq    buf(%rip), %rsi      /* reuse buf: load \n into first byte */
    movb    $'\n', (%rsi)
    movq    $1, %rdx
    int     $0x80

    jmp     1b

msg_hello:
    .ascii "hello, world from ring 3!\n"
    .set msg_hello_len, . - msg_hello

msg_prompt:
    .ascii "type a key: "
    .set msg_prompt_len, . - msg_prompt

msg_echo:
    .ascii "you typed: "
    .set msg_echo_len, . - msg_echo

.align 8
buf:
    .byte 0

.global syscall_user_program_end
syscall_user_program_end:

.global enter_user_mode
enter_user_mode:
    # rdi = user RIP, rsi = user RSP

    movw    $0x28, %ax         # load TSS (ring0 stack for interrupts)
    ltr     %ax

    pushq   $0x23              # SS  = user data | RPL=3
    pushq   %rsi               # RSP
    pushfq                     # RFLAGS (preserves reserved bits, then set IF)
    orq     $0x200, (%rsp)
    pushq   $0x1B              # CS  = user code | RPL=3
    pushq   %rdi               # RIP = user entry
    iretq

.section .data
.align 8
.global tss
tss:
    .skip 104
