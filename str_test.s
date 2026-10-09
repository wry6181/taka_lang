    # generated taka -> x86-64 (native, no C)
    .text
    .globl arena_mk
arena_mk:
    pushq %rbp
    movq %rsp, %rbp
    subq $112, %rsp
    movq %rdi, -72(%rbp)
    movq %rsi, -80(%rbp)
    movq %rdx, -88(%rbp)
    leaq -40(%rbp), %rdi
    movl $24, %ecx
    xorl %eax, %eax
    rep stosb
    movq -80(%rbp), %rax
    leaq -40(%rbp), %rcx
    movq %rax, (%rcx)
    movq -88(%rbp), %rax
    leaq -40(%rbp), %rcx
    addq $8, %rcx
    movq %rax, (%rcx)
    movl $0, %eax
    leaq -40(%rbp), %rcx
    addq $16, %rcx
    movq %rax, (%rcx)
    leaq -40(%rbp), %rax
    movq %rax, %rsi
    leaq -112(%rbp), %rdi
    movl $24, %ecx
    rep movsb
    leaq -112(%rbp), %rax
    movq %rax, %rsi
    movq -72(%rbp), %rdi
    movl $24, %ecx
    rep movsb
    jmp .Lfn0
.Lfn0:
    leave
    ret
    .globl arena_alloc
arena_alloc:
    pushq %rbp
    movq %rsp, %rbp
    subq $96, %rsp
    movq %rdi, -72(%rbp)
    movq %rsi, -80(%rbp)
    leaq -72(%rbp), %rax
    addq $16, %rax
    movq (%rax), %rax
    pushq %rax
    movl $7, %eax
    movq %rax, %rcx
    popq %rax
    addq %rcx, %rax
    pushq %rax
    movl $8, %eax
    movq %rax, %rcx
    popq %rax
    cqo
    idivq %rcx
    pushq %rax
    movl $8, %eax
    movq %rax, %rcx
    popq %rax
    imulq %rcx, %rax
    movq %rax, -88(%rbp)
    movq -88(%rbp), %rax
    pushq %rax
    movq -80(%rbp), %rax
    movq %rax, %rcx
    popq %rax
    addq %rcx, %rax
    movq %rax, -96(%rbp)
    movq -96(%rbp), %rax
    pushq %rax
    leaq -72(%rbp), %rax
    addq $8, %rax
    movq (%rax), %rax
    movq %rax, %rcx
    popq %rax
    cmpq %rcx, %rax
    seta %al
    movzbq %al, %rax
    testq %rax, %rax
    jz .L2
    xorl %eax, %eax
    jmp .Lfn1
    jmp .L3
.L2:
.L3:
    leaq -72(%rbp), %rax
    addq $16, %rax
    pushq %rax
    movq -96(%rbp), %rax
    popq %rcx
    movq %rax, (%rcx)
    leaq -72(%rbp), %rax
    movq (%rax), %rax
    pushq %rax
    movq -88(%rbp), %rax
    movq %rax, %rcx
    popq %rax
    addq %rcx, %rax
    jmp .Lfn1
.Lfn1:
    leave
    ret
    .globl str_eq
str_eq:
    pushq %rbp
    movq %rsp, %rbp
    subq $80, %rsp
    movq %rdi, -56(%rbp)
    movq %rsi, -72(%rbp)
    leaq -56(%rbp), %rax
    addq $8, %rax
    movq (%rax), %rax
    pushq %rax
    leaq -72(%rbp), %rax
    addq $8, %rax
    movq (%rax), %rax
    movq %rax, %rcx
    popq %rax
    cmpq %rcx, %rax
    setne %al
    movzbq %al, %rax
    testq %rax, %rax
    jz .L5
    movl $0, %eax
    jmp .Lfn4
    jmp .L6
.L5:
.L6:
    movl $0, %eax
    movq %rax, -80(%rbp)
.L7:
    movq -80(%rbp), %rax
    pushq %rax
    leaq -56(%rbp), %rax
    addq $8, %rax
    movq (%rax), %rax
    movq %rax, %rcx
    popq %rax
    cmpq %rcx, %rax
    setb %al
    movzbq %al, %rax
    testq %rax, %rax
    jz .L8
    leaq -56(%rbp), %rax
    pushq %rax
    movq -80(%rbp), %rax
    popq %rcx
    addq %rcx, %rax
    movsbq (%rax), %rax
    pushq %rax
    leaq -72(%rbp), %rax
    pushq %rax
    movq -80(%rbp), %rax
    popq %rcx
    addq %rcx, %rax
    movsbq (%rax), %rax
    movq %rax, %rcx
    popq %rax
    cmpl %ecx, %eax
    setne %al
    movzbq %al, %rax
    testq %rax, %rax
    jz .L9
    movl $0, %eax
    jmp .Lfn4
    jmp .L10
.L9:
.L10:
    leaq -80(%rbp), %rax
    pushq %rax
    movq (%rax), %rax
    popq %rcx
    pushq %rax
    addq $1, %rax
    movq %rax, (%rcx)
    popq %rax
    jmp .L7
.L8:
    movl $1, %eax
    jmp .Lfn4
.Lfn4:
    leave
    ret
    .globl i32_to_str
i32_to_str:
    pushq %rbp
    movq %rsp, %rbp
    subq $368, %rsp
    movl %edi, -272(%rbp)
    movq %rsi, -296(%rbp)
    movq -296(%rbp), %rax
    movq %rax, -184(%rbp)
    movl $12, %eax
    movq %rax, -248(%rbp)
    movq -184(%rbp), %rdi
    movq -248(%rbp), %rsi
    call arena_alloc
    movq %rax, -304(%rbp)
    movslq -272(%rbp), %rax
    pushq %rax
    movl $0, %eax
    movq %rax, %rcx
    popq %rax
    cmpl %ecx, %eax
    setl %al
    movzbq %al, %rax
    movb %al, -312(%rbp)
    movslq -272(%rbp), %rax
    movq %rax, -320(%rbp)
    movzbq -312(%rbp), %rax
    testq %rax, %rax
    jz .L12
    leaq -320(%rbp), %rax
    pushq %rax
    movl $0, %eax
    pushq %rax
    movslq -272(%rbp), %rax
    movq %rax, %rcx
    popq %rax
    subl %ecx, %eax
    popq %rcx
    movq %rax, (%rcx)
    jmp .L13
.L12:
.L13:
    movl $12, %eax
    movq %rax, -328(%rbp)
    movq -320(%rbp), %rax
    pushq %rax
    movl $0, %eax
    movq %rax, %rcx
    popq %rax
    cmpq %rcx, %rax
    sete %al
    movzbq %al, %rax
    testq %rax, %rax
    jz .L14
    leaq -328(%rbp), %rax
    pushq %rax
    movq -328(%rbp), %rax
    pushq %rax
    movl $1, %eax
    movq %rax, %rcx
    popq %rax
    subq %rcx, %rax
    popq %rcx
    movq %rax, (%rcx)
    leaq -304(%rbp), %rax
    pushq %rax
    movq -328(%rbp), %rax
    popq %rcx
    addq %rcx, %rax
    pushq %rax
    movl $48, %eax
    movzbq %al, %rax
    popq %rcx
    movb %al, (%rcx)
    jmp .L15
.L14:
.L15:
.L16:
    movq -320(%rbp), %rax
    pushq %rax
    movl $0, %eax
    movq %rax, %rcx
    popq %rax
    cmpq %rcx, %rax
    seta %al
    movzbq %al, %rax
    testq %rax, %rax
    jz .L17
    movq -320(%rbp), %rax
    pushq %rax
    movl $10, %eax
    movq %rax, %rcx
    popq %rax
    cqo
    idivq %rcx
    movq %rdx, %rax
    movq %rax, -336(%rbp)
    leaq -320(%rbp), %rax
    pushq %rax
    movq -320(%rbp), %rax
    pushq %rax
    movl $10, %eax
    movq %rax, %rcx
    popq %rax
    cqo
    idivq %rcx
    popq %rcx
    movq %rax, (%rcx)
    leaq -328(%rbp), %rax
    pushq %rax
    movq -328(%rbp), %rax
    pushq %rax
    movl $1, %eax
    movq %rax, %rcx
    popq %rax
    subq %rcx, %rax
    popq %rcx
    movq %rax, (%rcx)
    leaq -304(%rbp), %rax
    pushq %rax
    movq -328(%rbp), %rax
    popq %rcx
    addq %rcx, %rax
    pushq %rax
    movl $48, %eax
    pushq %rax
    movq -336(%rbp), %rax
    movq %rax, %rcx
    popq %rax
    addq %rcx, %rax
    movzbq %al, %rax
    popq %rcx
    movb %al, (%rcx)
    jmp .L16
.L17:
    movq -328(%rbp), %rax
    movq %rax, -344(%rbp)
    movzbq -312(%rbp), %rax
    testq %rax, %rax
    jz .L18
    leaq -344(%rbp), %rax
    pushq %rax
    movq -344(%rbp), %rax
    pushq %rax
    movl $1, %eax
    movq %rax, %rcx
    popq %rax
    subq %rcx, %rax
    popq %rcx
    movq %rax, (%rcx)
    leaq -304(%rbp), %rax
    pushq %rax
    movq -344(%rbp), %rax
    popq %rcx
    addq %rcx, %rax
    pushq %rax
    movl $45, %eax
    movzbq %al, %rax
    popq %rcx
    movb %al, (%rcx)
    jmp .L19
.L18:
.L19:
    movl $12, %eax
    pushq %rax
    movq -344(%rbp), %rax
    movq %rax, %rcx
    popq %rax
    subq %rcx, %rax
    movq %rax, -352(%rbp)
    movq -304(%rbp), %rax
    pushq %rax
    movq -344(%rbp), %rax
    movq %rax, %rcx
    popq %rax
    addq %rcx, %rax
    movq %rax, -104(%rbp)
    movq -352(%rbp), %rax
    movq %rax, -96(%rbp)
    leaq -104(%rbp), %rax
    movq %rax, -368(%rbp)
    movq -368(%rbp), %rax
    jmp .Lfn11
.Lfn11:
    leave
    ret
    .globl read_all
read_all:
    pushq %rbp
    movq %rsp, %rbp
    subq $544, %rsp
    movq %rdi, -144(%rbp)
    movq %rsi, -440(%rbp)
    movq %rdx, -464(%rbp)
    movq -440(%rbp), %rax
    movq %rax, -208(%rbp)
    leaq .Lstr_0(%rip), %rax
    movq %rax, -272(%rbp)
    movq -208(%rbp), %rdi
    movq -272(%rbp), %rsi
    call fopen@PLT
    movq %rax, -472(%rbp)
    movq -472(%rbp), %rax
    pushq %rax
    xorl %eax, %eax
    movq %rax, %rcx
    popq %rax
    cmpq %rcx, %rax
    sete %al
    movzbq %al, %rax
    testq %rax, %rax
    jz .L21
    leaq -56(%rbp), %rdi
    movl $16, %ecx
    xorl %eax, %eax
    rep stosb
    xorl %eax, %eax
    leaq -56(%rbp), %rcx
    movq %rax, (%rcx)
    movl $0, %eax
    leaq -56(%rbp), %rcx
    addq $8, %rcx
    movq %rax, (%rcx)
    leaq -56(%rbp), %rax
    movq %rax, %rsi
    leaq -488(%rbp), %rdi
    movl $16, %ecx
    rep movsb
    leaq -488(%rbp), %rax
    movq %rax, %rsi
    movq -144(%rbp), %rdi
    movl $16, %ecx
    rep movsb
    jmp .Lfn20
    jmp .L22
.L21:
.L22:
    movq -472(%rbp), %rax
    movq %rax, -208(%rbp)
    movl $0, %eax
    movq %rax, -272(%rbp)
    movl $2, %eax
    movl %eax, -336(%rbp)
    movq -208(%rbp), %rdi
    movq -272(%rbp), %rsi
    movl -336(%rbp), %edx
    call fseek@PLT
    movslq %eax, %rax
    movq -472(%rbp), %rax
    movq %rax, -208(%rbp)
    movq -208(%rbp), %rdi
    call ftell@PLT
    movq %rax, -496(%rbp)
    movq -472(%rbp), %rax
    movq %rax, -208(%rbp)
    movl $0, %eax
    movq %rax, -272(%rbp)
    movl $0, %eax
    movl %eax, -336(%rbp)
    movq -208(%rbp), %rdi
    movq -272(%rbp), %rsi
    movl -336(%rbp), %edx
    call fseek@PLT
    movslq %eax, %rax
    movq -496(%rbp), %rax
    movq %rax, -504(%rbp)
    movq -464(%rbp), %rax
    movq %rax, -208(%rbp)
    movq -504(%rbp), %rax
    movq %rax, -272(%rbp)
    movq -208(%rbp), %rdi
    movq -272(%rbp), %rsi
    call arena_alloc
    movq %rax, -512(%rbp)
    movq -512(%rbp), %rax
    movq %rax, -208(%rbp)
    movl $1, %eax
    movq %rax, -272(%rbp)
    movq -504(%rbp), %rax
    movq %rax, -336(%rbp)
    movq -472(%rbp), %rax
    movq %rax, -400(%rbp)
    movq -208(%rbp), %rdi
    movq -272(%rbp), %rsi
    movq -336(%rbp), %rdx
    movq -400(%rbp), %rcx
    call fread@PLT
    movq %rax, -520(%rbp)
    movq -472(%rbp), %rax
    movq %rax, -208(%rbp)
    movq -208(%rbp), %rdi
    call fclose@PLT
    movslq %eax, %rax
    leaq -120(%rbp), %rdi
    movl $16, %ecx
    xorl %eax, %eax
    rep stosb
    movq -512(%rbp), %rax
    leaq -120(%rbp), %rcx
    movq %rax, (%rcx)
    movq -520(%rbp), %rax
    leaq -120(%rbp), %rcx
    addq $8, %rcx
    movq %rax, (%rcx)
    leaq -120(%rbp), %rax
    movq %rax, %rsi
    leaq -536(%rbp), %rdi
    movl $16, %ecx
    rep movsb
    leaq -536(%rbp), %rax
    movq %rax, %rsi
    movq -144(%rbp), %rdi
    movl $16, %ecx
    rep movsb
    jmp .Lfn20
.Lfn20:
    leave
    ret
    .globl taka_main
taka_main:
    pushq %rbp
    movq %rsp, %rbp
    subq $784, %rsp
    movl $512, %eax
    movq %rax, -368(%rbp)
    movq -368(%rbp), %rdi
    call malloc@PLT
    movq %rax, -600(%rbp)
    movq -600(%rbp), %rax
    movq %rax, -368(%rbp)
    movl $512, %eax
    movq %rax, -432(%rbp)
    leaq -32(%rbp), %rdi
    movq -368(%rbp), %rsi
    movq -432(%rbp), %rdx
    call arena_mk
    leaq -32(%rbp), %rax
    movq %rax, %rsi
    leaq -624(%rbp), %rdi
    movl $24, %ecx
    rep movsb
    movl $0, %eax
    movl %eax, -368(%rbp)
    leaq -624(%rbp), %rax
    movq %rax, -432(%rbp)
    movl -368(%rbp), %edi
    movq -432(%rbp), %rsi
    call i32_to_str
    movq %rax, -640(%rbp)
    movl $42, %eax
    movl %eax, -368(%rbp)
    leaq -624(%rbp), %rax
    movq %rax, -432(%rbp)
    movl -368(%rbp), %edi
    movq -432(%rbp), %rsi
    call i32_to_str
    movq %rax, -656(%rbp)
    movl $0, %eax
    pushq %rax
    movl $7, %eax
    movq %rax, %rcx
    popq %rax
    subl %ecx, %eax
    movl %eax, -368(%rbp)
    leaq -624(%rbp), %rax
    movq %rax, -432(%rbp)
    movl -368(%rbp), %edi
    movq -432(%rbp), %rsi
    call i32_to_str
    movq %rax, -672(%rbp)
    movq -640(%rbp), %rax
    movq %rax, -368(%rbp)
    leaq .Lstr_2(%rip), %rax
    movq %rax, -120(%rbp)
    movl $1, %eax
    movq %rax, -112(%rbp)
    leaq -120(%rbp), %rax
    movq %rax, -432(%rbp)
    movq -368(%rbp), %rdi
    movq -432(%rbp), %rsi
    call str_eq
    movslq %eax, %rax
    pushq %rax
    leaq .Lstr_1(%rip), %rdi
    popq %rsi
    xorl %eax, %eax
    call printf@PLT
    movq -656(%rbp), %rax
    movq %rax, -368(%rbp)
    leaq .Lstr_3(%rip), %rax
    movq %rax, -136(%rbp)
    movl $2, %eax
    movq %rax, -128(%rbp)
    leaq -136(%rbp), %rax
    movq %rax, -432(%rbp)
    movq -368(%rbp), %rdi
    movq -432(%rbp), %rsi
    call str_eq
    movslq %eax, %rax
    pushq %rax
    leaq .Lstr_1(%rip), %rdi
    popq %rsi
    xorl %eax, %eax
    call printf@PLT
    movq -672(%rbp), %rax
    movq %rax, -368(%rbp)
    leaq .Lstr_4(%rip), %rax
    movq %rax, -152(%rbp)
    movl $2, %eax
    movq %rax, -144(%rbp)
    leaq -152(%rbp), %rax
    movq %rax, -432(%rbp)
    movq -368(%rbp), %rdi
    movq -432(%rbp), %rsi
    call str_eq
    movslq %eax, %rax
    pushq %rax
    leaq .Lstr_1(%rip), %rdi
    popq %rsi
    xorl %eax, %eax
    call printf@PLT
    movq -656(%rbp), %rax
    movq %rax, -368(%rbp)
    movq -672(%rbp), %rax
    movq %rax, -432(%rbp)
    movq -368(%rbp), %rdi
    movq -432(%rbp), %rsi
    call str_eq
    movslq %eax, %rax
    pushq %rax
    leaq .Lstr_1(%rip), %rdi
    popq %rsi
    xorl %eax, %eax
    call printf@PLT
    leaq .Lstr_5(%rip), %rax
    movq %rax, -368(%rbp)
    leaq .Lstr_6(%rip), %rax
    movq %rax, -432(%rbp)
    movq -368(%rbp), %rdi
    movq -432(%rbp), %rsi
    call fopen@PLT
    movq %rax, -680(%rbp)
    movq $0, -704(%rbp)
    movq $0, -696(%rbp)
    movq $0, -688(%rbp)
    movq $3, %rdi
    call malloc@PLT
    movq %rax, -704(%rbp)
    testq %rax, %rax
    jnz .L24
    movl $1, %edi
    call exit@PLT
.L24:
    movq $3, -696(%rbp)
    movq $3, -688(%rbp)
    movl $104, %eax
    movzbq %al, %rax
    movq -704(%rbp), %rcx
    movb %al, 0(%rcx)
    movl $105, %eax
    movzbq %al, %rax
    movq -704(%rbp), %rcx
    movb %al, 1(%rcx)
    movl $32, %eax
    movzbq %al, %rax
    movq -704(%rbp), %rcx
    movb %al, 2(%rcx)
    movl $42, %eax
    movl %eax, -368(%rbp)
    leaq -624(%rbp), %rax
    movq %rax, -432(%rbp)
    movl -368(%rbp), %edi
    movq -432(%rbp), %rsi
    call i32_to_str
    movq %rax, -720(%rbp)
    movl $0, %eax
    movq %rax, -728(%rbp)
.L25:
    movq -728(%rbp), %rax
    pushq %rax
    leaq -720(%rbp), %rax
    addq $8, %rax
    movq (%rax), %rax
    movq %rax, %rcx
    popq %rax
    cmpq %rcx, %rax
    setb %al
    movzbq %al, %rax
    testq %rax, %rax
    jz .L26
    leaq -704(%rbp), %rax
    movq %rax, -312(%rbp)
    leaq -720(%rbp), %rax
    pushq %rax
    movq -728(%rbp), %rax
    popq %rcx
    addq %rcx, %rax
    movsbq (%rax), %rax
    movb %al, -432(%rbp)
    movq -432(%rbp), %rsi
    movq -312(%rbp), %rdi
    call List_u8_push
    leaq -728(%rbp), %rax
    pushq %rax
    movq -728(%rbp), %rax
    pushq %rax
    movl $1, %eax
    movq %rax, %rcx
    popq %rax
    addq %rcx, %rax
    popq %rcx
    movq %rax, (%rcx)
    jmp .L25
.L26:
    leaq -704(%rbp), %rax
    movq %rax, -312(%rbp)
    movl $10, %eax
    movb %al, -432(%rbp)
    movq -432(%rbp), %rsi
    movq -312(%rbp), %rdi
    call List_u8_push
    leaq -704(%rbp), %rax
    movq (%rax), %rax
    movq %rax, -368(%rbp)
    movl $1, %eax
    movq %rax, -432(%rbp)
    leaq -704(%rbp), %rax
    addq $8, %rax
    movq (%rax), %rax
    movq %rax, -496(%rbp)
    movq -680(%rbp), %rax
    movq %rax, -560(%rbp)
    movq -368(%rbp), %rdi
    movq -432(%rbp), %rsi
    movq -496(%rbp), %rdx
    movq -560(%rbp), %rcx
    call fwrite@PLT
    movq -680(%rbp), %rax
    movq %rax, -368(%rbp)
    movq -368(%rbp), %rdi
    call fclose@PLT
    movslq %eax, %rax
    leaq .Lstr_5(%rip), %rax
    movq %rax, -368(%rbp)
    leaq -624(%rbp), %rax
    movq %rax, -432(%rbp)
    leaq -224(%rbp), %rdi
    movq -368(%rbp), %rsi
    movq -432(%rbp), %rdx
    call read_all
    leaq -224(%rbp), %rax
    movq %rax, %rsi
    leaq -744(%rbp), %rdi
    movl $16, %ecx
    rep movsb
    leaq -744(%rbp), %rax
    addq $8, %rax
    movq (%rax), %rax
    movslq %eax, %rax
    pushq %rax
    leaq .Lstr_1(%rip), %rdi
    popq %rsi
    xorl %eax, %eax
    call printf@PLT
    leaq .Lstr_7(%rip), %rax
    movq %rax, -256(%rbp)
    movl $6, %eax
    movq %rax, -248(%rbp)
    leaq -256(%rbp), %rax
    movq %rax, -760(%rbp)
    leaq -744(%rbp), %rax
    movq (%rax), %rax
    movq %rax, -288(%rbp)
    leaq -744(%rbp), %rax
    addq $8, %rax
    movq (%rax), %rax
    movq %rax, -280(%rbp)
    leaq -288(%rbp), %rax
    movq %rax, -776(%rbp)
    movq -776(%rbp), %rax
    movq %rax, -368(%rbp)
    movq -760(%rbp), %rax
    movq %rax, -432(%rbp)
    movq -368(%rbp), %rdi
    movq -432(%rbp), %rsi
    call str_eq
    movslq %eax, %rax
    pushq %rax
    leaq .Lstr_1(%rip), %rdi
    popq %rsi
    xorl %eax, %eax
    call printf@PLT
    movq -600(%rbp), %rax
    movq %rax, -368(%rbp)
    movq -368(%rbp), %rdi
    call free@PLT
    jmp .Lfn23
.Lfn23:
    leave
    ret
List_u8_push:
    pushq %rbp
    movq %rsp, %rbp
    subq $32, %rsp
    movq %rdi, -8(%rbp)
    movq %rsi, -16(%rbp)
    movq -8(%rbp), %rax
    movq 8(%rax), %rax
    movq -8(%rbp), %rcx
    cmpq 16(%rcx), %rax
    jne .L27
    movq -8(%rbp), %rax
    movq 16(%rax), %rsi
    testq %rsi, %rsi
    jnz .L28
    movq $8, %rsi
    jmp .L29
.L28:
    leaq (%rsi,%rsi), %rsi
.L29:
    imulq $1, %rsi
    movq -8(%rbp), %rax
    movq (%rax), %rdi
    call realloc@PLT
    testq %rax, %rax
    jnz .L30
    movl $1, %edi
    call exit@PLT
.L30:
    movq -8(%rbp), %rcx
    movq %rax, (%rcx)
    movq 16(%rcx), %rsi
    testq %rsi, %rsi
    jnz .L31
    movq $8, %rsi
    jmp .L32
.L31:
    leaq (%rsi,%rsi), %rsi
.L32:
    movq %rsi, 16(%rcx)
.L27:
    movq -8(%rbp), %rax
    movq (%rax), %rax
    movq -8(%rbp), %rcx
    movq 8(%rcx), %rcx
    addq %rcx, %rax
    movq %rax, -24(%rbp)
    movq -16(%rbp), %rax
    movq -24(%rbp), %rcx
    movb %al, (%rcx)
    movq -8(%rbp), %rax
    incq 8(%rax)
    leave
    ret
    .globl main
main:
    pushq %rbp
    movq %rsp, %rbp
    call taka_main
    xorl %eax, %eax
    leave
    ret
    .section .rodata
.Lstr_0:
    .asciz "r"
.Lstr_1:
    .asciz "%d\n"
.Lstr_2:
    .asciz "0"
.Lstr_3:
    .asciz "42"
.Lstr_4:
    .asciz "-7"
.Lstr_5:
    .asciz "str_out.txt"
.Lstr_6:
    .asciz "w"
.Lstr_7:
    .asciz "hi 42\n"
    .section .note.GNU-stack,"",@progbits
