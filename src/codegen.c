#include "codegen.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/stat.h>

/*
 * ELF64 layout (Linux x86-64, static, no libc):
 *   0x00  ELF header       (64 bytes)
 *   0x40  Program header   (56 bytes) PT_LOAD
 *   0x78  .text            (machine code)
 *           nodes + exit(12) + [print_u64 helper(156)]
 *   .text+  .rodata         (string literals)
 */
#define LOAD_ADDR  ((uint64_t)0x400000)
#define HDR_SIZE   64U
#define PHDR_SIZE  56U
#define TEXT_OFF   (HDR_SIZE + PHDR_SIZE)
#define CODE_VA    (LOAD_ADDR + TEXT_OFF)

/* ------------------------------------------------------------------ */
/* Dynamic byte buffer                                                 */
/* ------------------------------------------------------------------ */

typedef struct { uint8_t *data; size_t len, cap; } Buf;

static void buf_push(Buf *b, const uint8_t *bytes, size_t n) {
    if (b->len + n > b->cap) {
        b->cap = (b->cap + n + 64) * 2;
        b->data = realloc(b->data, b->cap);
    }
    memcpy(b->data + b->len, bytes, n);
    b->len += n;
}
#define PUSH(b, ...) do { uint8_t _t[]={__VA_ARGS__}; buf_push(b,_t,sizeof(_t)); } while(0)

static void push32(Buf *b, uint32_t v) {
    uint8_t t[4] = { v, v>>8, v>>16, v>>24 };
    buf_push(b, t, 4);
}
static void patch32(Buf *b, size_t pos, int32_t v) {
    b->data[pos]  =(uint8_t)(v);
    b->data[pos+1]=(uint8_t)(v>>8);
    b->data[pos+2]=(uint8_t)(v>>16);
    b->data[pos+3]=(uint8_t)(v>>24);
}
static void elf16(uint8_t *b,size_t p,uint16_t v){b[p]=v;b[p+1]=v>>8;}
static void elf32(uint8_t *b,size_t p,uint32_t v){b[p]=v;b[p+1]=v>>8;b[p+2]=v>>16;b[p+3]=v>>24;}
static void elf64(uint8_t *b,size_t p,uint64_t v){for(int j=0;j<8;j++)b[p+j]=(v>>(j*8))&0xff;}

/* ------------------------------------------------------------------ */
/* print_u64 subroutine — 156 bytes, embedded in code section.
 *
 * Called with: rax = nanoseconds value
 * Outputs:     "time: <decimal> ns\n" to stdout
 * Preserves:   rbx, rbp, r12-r15  (does NOT clobber loop registers)
 * Clobbers:    rax, rcx, rdx, rsi, rdi, r8, r9, r10, r11
 *
 * Layout (verified byte-by-byte):
 *   0-7    save rbx, move rax→rbx, allocate 24-byte stack buffer
 *   8-33   sys_write("time: ", 6)  via rip-relative lea
 *   34-94  digit extraction loop into [rsp..rsp+23], r8=count, r9=cursor
 *   95-115 sys_write(digits)
 *   116-139 sys_write(" ns\n", 4) via rip-relative lea
 *   140-145 restore stack, pop rbx, ret
 *   146-151 "time: "
 *   152-155 " ns\n"
 * ------------------------------------------------------------------ */
static const uint8_t print_u64_code[156] = {
/*  0 */ 0x53,                                            // push rbx
/*  1 */ 0x48,0x89,0xC3,                                  // mov rbx, rax
/*  4 */ 0x48,0x83,0xEC,0x18,                             // sub rsp, 24
/* write "time: " --------------------------------------------------- */
/*  8 */ 0x48,0x8D,0x35, 0x83,0x00,0x00,0x00,            // lea rsi,[rip+131] →146
/* 15 */ 0xB8,0x01,0x00,0x00,0x00,                        // mov eax, 1
/* 20 */ 0xBF,0x01,0x00,0x00,0x00,                        // mov edi, 1
/* 25 */ 0xBA,0x06,0x00,0x00,0x00,                        // mov edx, 6
/* 30 */ 0x0F,0x05,                                       // syscall
/* digit extraction ------------------------------------------------- */
/* 32 */ 0x4D,0x31,0xC0,                                  // xor r8, r8
/* 35 */ 0x4C,0x8D,0x4C,0x24,0x17,                       // lea r9, [rsp+23]
/* 40 */ 0x48,0x85,0xDB,                                  // test rbx, rbx
/* 43 */ 0x75,0x0C,                                       // jnz +12 →57  (non-zero → digit loop)
/* 45 */ 0x41,0xC6,0x01,0x30,                             // mov byte [r9], '0'
/* 49 */ 0x49,0xFF,0xC9,                                  // dec r9
/* 52 */ 0x49,0xFF,0xC0,                                  // inc r8
/* 55 */ 0xEB,0x26,                                       // jmp +38 →95
/* digit loop ------------------------------------------------------- */
/* 57 */ 0x48,0x85,0xDB,                                  // test rbx, rbx
/* 60 */ 0x74,0x21,                                       // jz +33 →95
/* 62 */ 0x48,0x89,0xD8,                                  // mov rax, rbx
/* 65 */ 0x48,0x31,0xD2,                                  // xor rdx, rdx
/* 68 */ 0x48,0xC7,0xC1,0x0A,0x00,0x00,0x00,             // mov rcx, 10
/* 75 */ 0x48,0xF7,0xF1,                                  // div rcx
/* 78 */ 0x48,0x89,0xC3,                                  // mov rbx, rax
/* 81 */ 0x80,0xC2,0x30,                                  // add dl, '0'
/* 84 */ 0x41,0x88,0x11,                                  // mov byte [r9], dl
/* 87 */ 0x49,0xFF,0xC9,                                  // dec r9
/* 90 */ 0x49,0xFF,0xC0,                                  // inc r8
/* 93 */ 0xEB,0xDA,                                       // jmp -38 →57
/* emit digits ----------------------------------------------------- */
/* 95 */ 0x49,0xFF,0xC1,                                  // inc r9
/* 98 */ 0xB8,0x01,0x00,0x00,0x00,                        // mov eax, 1
/*103 */ 0xBF,0x01,0x00,0x00,0x00,                        // mov edi, 1
/*108 */ 0x4C,0x89,0xCE,                                  // mov rsi, r9
/*111 */ 0x4C,0x89,0xC2,                                  // mov rdx, r8
/*114 */ 0x0F,0x05,                                       // syscall
/* write " ns\n" ---------------------------------------------------- */
/*116 */ 0x48,0x8D,0x35, 0x1D,0x00,0x00,0x00,            // lea rsi,[rip+29] →152
/*123 */ 0xB8,0x01,0x00,0x00,0x00,                        // mov eax, 1
/*128 */ 0xBF,0x01,0x00,0x00,0x00,                        // mov edi, 1
/*133 */ 0xBA,0x04,0x00,0x00,0x00,                        // mov edx, 4
/*138 */ 0x0F,0x05,                                       // syscall
/* epilogue --------------------------------------------------------- */
/*140 */ 0x48,0x83,0xC4,0x18,                             // add rsp, 24
/*144 */ 0x5B,                                            // pop rbx
/*145 */ 0xC3,                                            // ret
/* data ------------------------------------------------------------- */
/*146 */ 't','i','m','e',':',' ',                          // "time: "
/*152 */ ' ','n','s','\n',                                 // " ns\n"
};

/* ------------------------------------------------------------------ */
/* For loop register table (r12..r15, 4 nesting levels)               */
/* ------------------------------------------------------------------ */
static const uint8_t reg_mov_b[4] = { 0xBC,0xBD,0xBE,0xBF };
static const uint8_t reg_cmp_m[4] = { 0xFC,0xFD,0xFE,0xFF };
static const uint8_t reg_inc_m[4] = { 0xC4,0xC5,0xC6,0xC7 };

/* ------------------------------------------------------------------ */
/* Size measurement (must exactly match emission)                      */
/* ------------------------------------------------------------------ */

static size_t measure_nodes(const Node *nodes, size_t count, int depth);

static size_t measure_node(const Node *n, int depth) {
    if (n->kind == NODE_ECHO) return 30;
    if (n->kind == NODE_FOR) {
        size_t body = measure_nodes(n->for_body, n->for_body_count, depth+1);
        return 6 + 7 + 6 + body + 3 + 5;
    }
    if (n->kind == NODE_TIME) {
        size_t body = measure_nodes(n->for_body, n->for_body_count, depth);
        /* preamble(21) + body + postamble(54) */
        return 21 + body + 54;
    }
    return 0;
}
static size_t measure_nodes(const Node *nodes, size_t count, int depth) {
    size_t total = 0;
    for (size_t i = 0; i < count; i++) total += measure_node(&nodes[i], depth);
    return total;
}

/* Check if any NODE_TIME exists (recursive) */
static int has_time_node(const Node *nodes, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (nodes[i].kind == NODE_TIME) return 1;
        if (nodes[i].for_body_count &&
            has_time_node(nodes[i].for_body, nodes[i].for_body_count))
            return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* String VA pre-pass                                                  */
/* ------------------------------------------------------------------ */
static void assign_str_vas(Node *nodes, size_t count,
                           Buf *str_data, uint64_t str_base_va) {
    for (size_t i = 0; i < count; i++) {
        Node *n = &nodes[i];
        if (n->kind == NODE_ECHO) {
            n->str_va = str_base_va + str_data->len;
            buf_push(str_data, (const uint8_t*)n->str_val, n->str_len);
        } else if (n->for_body_count) {
            assign_str_vas(n->for_body, n->for_body_count, str_data, str_base_va);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Code emission                                                       */
/* ------------------------------------------------------------------ */

static void emit_nodes(Buf *code, Node *nodes, size_t count,
                       int depth, uint64_t print_u64_va);

/* sys_write(1, str_va, len) — fixed 30 bytes */
static void emit_write(Buf *code, uint64_t str_va, size_t str_len) {
    size_t base = code->len;
    int32_t disp = (int32_t)((int64_t)str_va - (int64_t)(CODE_VA + base + 21));
    PUSH(code, 0x48,0xC7,0xC0, 0x01,0x00,0x00,0x00);  /* mov rax,1 */
    PUSH(code, 0x48,0xC7,0xC7, 0x01,0x00,0x00,0x00);  /* mov rdi,1 */
    PUSH(code, 0x48,0x8D,0x35,                          /* lea rsi,[rip+disp] */
         (uint8_t)disp,(uint8_t)(disp>>8),
         (uint8_t)(disp>>16),(uint8_t)(disp>>24));
    uint32_t rlen = (uint32_t)str_len;
    PUSH(code, 0x48,0xC7,0xC2,                          /* mov rdx,len */
         (uint8_t)rlen,(uint8_t)(rlen>>8),
         (uint8_t)(rlen>>16),(uint8_t)(rlen>>24));
    PUSH(code, 0x0F,0x05);                              /* syscall */
}

/* for loop using register at depth (0=r12..3=r15) */
static void emit_for(Buf *code, Node *n, int depth, uint64_t print_u64_va) {
    if (depth > 3) depth = 3;

    PUSH(code, 0x41, reg_mov_b[depth]);                 /* mov r1xd, from */
    push32(code, (uint32_t)(int32_t)n->for_from);

    size_t loop_top = code->len;

    PUSH(code, 0x49,0x81, reg_cmp_m[depth]);            /* cmp r1x, to */
    push32(code, (uint32_t)(int32_t)n->for_to);

    PUSH(code, 0x0F,0x8F);                              /* jg loop_end */
    size_t jg_patch = code->len;
    push32(code, 0);

    emit_nodes(code, n->for_body, n->for_body_count, depth+1, print_u64_va);

    PUSH(code, 0x49,0xFF, reg_inc_m[depth]);            /* inc r1x */

    PUSH(code, 0xE9);                                   /* jmp loop_top */
    int32_t back = (int32_t)((int64_t)loop_top - (int64_t)(code->len + 4));
    push32(code, (uint32_t)back);

    int32_t fwd = (int32_t)((int64_t)code->len - (int64_t)(jg_patch + 4));
    patch32(code, jg_patch, fwd);
}

/*
 * time block — measures body execution with CLOCK_MONOTONIC.
 *
 * Stack layout (sub rsp, 32):
 *   [rsp+16..rsp+31]  start timespec  (tv_sec=8B, tv_nsec=8B)
 *   [rsp+0 ..rsp+15]  end   timespec
 *
 * Preamble (21 bytes):
 *   sub rsp,32  | mov eax,228 | mov edi,1 | lea rsi,[rsp+16] | syscall
 *
 * Postamble (54 bytes):
 *   mov eax,228 | mov edi,1 | lea rsi,[rsp] | syscall
 *   mov rax,[rsp] | sub rax,[rsp+16] | imul rax,rax,1e9
 *   mov rcx,[rsp+8] | sub rcx,[rsp+24] | add rax,rcx
 *   call print_u64  | add rsp,32
 */
static void emit_time(Buf *code, Node *n, int depth, uint64_t print_u64_va) {
    /* --- preamble --- */
    PUSH(code, 0x48,0x83,0xEC,0x20);                   /* sub rsp,32       [4]  */
    PUSH(code, 0xB8,0xE4,0x00,0x00,0x00);              /* mov eax,228      [5]  */
    PUSH(code, 0xBF,0x01,0x00,0x00,0x00);              /* mov edi,1        [5]  */
    PUSH(code, 0x48,0x8D,0x74,0x24,0x10);              /* lea rsi,[rsp+16] [5]  */
    PUSH(code, 0x0F,0x05);                              /* syscall          [2] =21 */

    /* --- body --- */
    emit_nodes(code, n->for_body, n->for_body_count, depth, print_u64_va);

    /* --- second clock_gettime(CLOCK_MONOTONIC, &end) --- */
    PUSH(code, 0xB8,0xE4,0x00,0x00,0x00);              /* mov eax,228      [5]  */
    PUSH(code, 0xBF,0x01,0x00,0x00,0x00);              /* mov edi,1        [5]  */
    PUSH(code, 0x48,0x8D,0x34,0x24);                   /* lea rsi,[rsp]    [4]  */
    PUSH(code, 0x0F,0x05);                              /* syscall          [2] =16 */

    /* --- diff: rax = (end.sec - start.sec)*1e9 + (end.nsec - start.nsec) --- */
    PUSH(code, 0x48,0x8B,0x04,0x24);                   /* mov rax,[rsp]    [4]  end.sec  */
    PUSH(code, 0x48,0x2B,0x44,0x24,0x10);              /* sub rax,[rsp+16] [5]  -start.sec */
    PUSH(code, 0x48,0x69,0xC0,0x00,0xCA,0x9A,0x3B);   /* imul rax,rax,1e9 [7]  *1000000000 */
    PUSH(code, 0x48,0x8B,0x4C,0x24,0x08);              /* mov rcx,[rsp+8]  [5]  end.nsec */
    PUSH(code, 0x48,0x2B,0x4C,0x24,0x18);              /* sub rcx,[rsp+24] [5]  -start.nsec */
    PUSH(code, 0x48,0x01,0xC8);                        /* add rax,rcx      [3] =29 */

    /* --- call print_u64 --- */
    PUSH(code, 0xE8);                                   /* call rel32       [1] */
    int32_t disp = (int32_t)((int64_t)print_u64_va
                             - (int64_t)(CODE_VA + code->len + 4));
    push32(code, (uint32_t)disp);                       /*                  [4] =5 */

    PUSH(code, 0x48,0x83,0xC4,0x20);                   /* add rsp,32       [4] =54 total postamble */
}

static void emit_nodes(Buf *code, Node *nodes, size_t count,
                       int depth, uint64_t print_u64_va) {
    for (size_t i = 0; i < count; i++) {
        Node *n = &nodes[i];
        if      (n->kind == NODE_ECHO) emit_write(code, n->str_va, n->str_len);
        else if (n->kind == NODE_FOR)  emit_for(code, n, depth, print_u64_va);
        else if (n->kind == NODE_TIME) emit_time(code, n, depth, print_u64_va);
    }
}

static void emit_exit(Buf *code) {
    PUSH(code, 0x48,0xC7,0xC0, 0x3C,0x00,0x00,0x00);  /* mov rax,60 */
    PUSH(code, 0x48,0x31,0xFF);                         /* xor rdi,rdi */
    PUSH(code, 0x0F,0x05);                              /* syscall    */
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int codegen_emit_elf(Arena *arena, const AST *ast, const char *out_path) {
    (void)arena;

    size_t main_code = measure_nodes(ast->nodes, ast->count, 0);
    int    need_helper = has_time_node(ast->nodes, ast->count);

    /* print_u64 lives right after exit (12 bytes) */
    uint64_t print_u64_va = CODE_VA + main_code + 12;

    /* String data starts after code (+helper if needed) */
    size_t helper_size   = need_helper ? sizeof(print_u64_code) : 0;
    uint64_t str_base_va = print_u64_va + helper_size;

    /* Pre-pass: assign string VAs */
    Buf str_data = {0};
    assign_str_vas(ast->nodes, ast->count, &str_data, str_base_va);

    /* Emit code */
    Buf code = {0};
    emit_nodes(&code, ast->nodes, ast->count, 0, print_u64_va);
    emit_exit(&code);
    if (need_helper)
        buf_push(&code, print_u64_code, sizeof(print_u64_code));

    if (code.len != main_code + 12 + helper_size) {
        fprintf(stderr,
            "codegen: size mismatch! expected=%zu emitted=%zu\n",
            main_code + 12 + helper_size, code.len);
        free(code.data); free(str_data.data);
        return 1;
    }

    /* Build ELF */
    size_t file_size = TEXT_OFF + code.len + str_data.len;
    uint8_t *elf = calloc(1, file_size);

    elf[0]=0x7f; elf[1]='E'; elf[2]='L'; elf[3]='F';
    elf[4]=2; elf[5]=1; elf[6]=1; elf[7]=0;
    elf16(elf,16,2); elf16(elf,18,62); elf32(elf,20,1);
    elf64(elf,24,CODE_VA); elf64(elf,32,HDR_SIZE);
    elf64(elf,40,0); elf32(elf,48,0);
    elf16(elf,52,HDR_SIZE); elf16(elf,54,PHDR_SIZE);
    elf16(elf,56,1); elf16(elf,58,64); elf16(elf,60,0); elf16(elf,62,0);

    size_t ph = HDR_SIZE;
    elf32(elf,ph+0,1); elf32(elf,ph+4,5);
    elf64(elf,ph+8,0); elf64(elf,ph+16,LOAD_ADDR); elf64(elf,ph+24,LOAD_ADDR);
    elf64(elf,ph+32,(uint64_t)file_size); elf64(elf,ph+40,(uint64_t)file_size);
    elf64(elf,ph+48,0x200000UL);

    memcpy(elf + TEXT_OFF,            code.data,     code.len);
    memcpy(elf + TEXT_OFF + code.len, str_data.data, str_data.len);

    FILE *f = fopen(out_path, "wb");
    if (!f) { perror(out_path); free(elf); free(code.data); free(str_data.data); return 1; }
    fwrite(elf, 1, file_size, f);
    fclose(f);
    chmod(out_path, 0755);

    free(elf); free(code.data); free(str_data.data);
    return 0;
}
