/* forth.c — LibOS #1 LispOS: интерпретатор Forth (этап M1, BIOS/x86 32-bit).
 *
 * Архитектура: indirect-threading Forth.
 *   - Словарь — хеш-таблица Word{name, xt, param}.
 *   - Тело `: ... ;` — массив Word* в статическом пуле тел; литералы — LIT.
 *   - IF/ELSE/THEN, BEGIN..UNTIL, BEGIN..WHILE..REPEAT компилируются в
 *     jump-слова с patch целевого индекса в теле.
 *   - Вывод/ввод — exokernel syscall ABI (libos/syscall.h): TTY_WRITE/TTY_READ.
 *
 * Память: bump-heap фиксированного размера + статика; malloc нет —
 * утечки невозможны по построению (аудит agent.md §правила).
 */
#include <stdint.h>
#include "libos/syscall.h"

typedef int32_t cell;
typedef uint32_t ucell;

/* ============================ память ============================ */
#define HEAP_SIZE (48u * 1024u)
static unsigned char heap[HEAP_SIZE] __attribute__((aligned(16)));
static ucell brk;

static void *halloc(ucell n)
{
    n = (n + 15u) & ~15u;
    if (brk + n > HEAP_SIZE) return (void *)0;
    void *p = heap + brk;
    brk += n;
    return p;
}

static unsigned str_len(const char *s){ unsigned n=0; while(s[n]) n++; return n; }
static unsigned mem_eq(const void *a,const void *b,unsigned n){
    const unsigned char *x=a,*y=b; for(unsigned i=0;i<n;i++) if(x[i]!=y[i]) return 0; return 1; }

/* ============================ tty через syscall ABI ============================ */
static void tty_out(const char *s, unsigned len){
    kernel_syscall(EX_SYS_TTY_WRITE,(uint32_t)(uintptr_t)s,len,0); }
static void tty_str(const char *s){ tty_out(s,str_len(s)); }
static void emit(char c){ tty_out(&c,1); }
static void crlf(void){ tty_out("\r\n",2); }

/* блокирующее чтение символа: polling TTY_READ */
static int get_char_blocking(void){
    char c;
    for(;;){
        if(kernel_syscall(EX_SYS_TTY_READ,(uint32_t)(uintptr_t)&c,1,0)==1)
            return (unsigned char)c;
        __asm__ volatile("pause");
    }
}

/* ============================ стек данных ============================ */
#define STACK_DEPTH 96
static cell dstack[STACK_DEPTH];
static int dsp = -1;
static void push(cell v){
    if(dsp+1>=STACK_DEPTH){ tty_str("\r\n!! stack overflow"); return; }
    dstack[++dsp]=v; }
static cell pop(void){
    if(dsp<0){ tty_str("\r\n!! stack underflow"); return 0; }
    return dstack[dsp--]; }
static cell peek(int below){ int i=dsp-below; return i>=0?dstack[i]:0; }

/* ============================ словарь ============================ */
typedef struct Word Word;
typedef void (*xt_t)(Word *w);
struct Word {
    Word *next;      /* hash chain */
    char *name;
    uint8_t nlen;
    uint8_t immediate;
    xt_t  xt;
    cell  param;     /* LIT: значение; VARIABLE: адрес; ColonRef: индекс тела */
};

#define HASH_N 199
static Word *hashtab[HASH_N];

static ucell hash_name(const char *s, unsigned n){
    ucell h=5381;
    for(unsigned i=0;i<n;i++){ char c=s[i];
        if(c>='a'&&c<='z') c=(char)(c-32);
        h=h*33u+(unsigned char)c; }
    return h%HASH_N;
}
static Word *find(const char *name, unsigned n){
    for(Word *w=hashtab[hash_name(name,n)]; w; w=w->next)
        if(w->nlen==n && mem_eq(w->name,name,n)) return w;
    return (Word*)0;
}
static Word *create(const char *name, unsigned n){
    Word *w=halloc(sizeof(Word)); char *nm=halloc(n+1);
    if(!w||!nm){ tty_str("\r\n!! out of memory"); return (Word*)0; }
    for(unsigned i=0;i<n;i++){ char c=name[i];
        nm[i]=(c>='a'&&c<='z')?(char)(c-32):c; }
    nm[n]=0;
    w->name=nm; w->nlen=(uint8_t)n; w->xt=(xt_t)0; w->param=0; w->immediate=0;
    ucell h=hash_name(name,n); w->next=hashtab[h]; hashtab[h]=w;
    return w;
}
static void add_builtin(const char *name, xt_t fn, int imm){
    Word *w=create(name,str_len(name));
    if(w){ w->xt=fn; w->immediate=(uint8_t)imm; }
}

/* ============================ тела слов ============================ */
#define MAX_BODIES 128
#define BODY_MAX   96
static Word *bodies[MAX_BODIES][BODY_MAX];
static int next_body;

/* Интерпретатор: единый цикл, кадры (idx,i) на frames[]. Вызов слова — push
 * кадра; возврат из тела — pop. Никакой C-рекурсии => никаких стековых ловушек. */
static int g_jump; static int g_jump_target;
typedef struct { int idx; int i; } Frame;
static Frame frames[64]; static int fsp=-1;

static void enter_body(int idx){
    if(fsp+1>=(int)(sizeof(frames)/sizeof(frames[0]))){
        tty_str("\r\n!! recursion too deep"); return; }
    frames[++fsp].idx=idx; frames[fsp].i=0;
}
/* целевой индекс прыжка может указывать на тело внешнего вызова (BEGIN..WHILE
 * внутри IF): интерпретируем param как (body<<8)|idx и ставим кадр при необходимости */
static void set_ip(int key_target){
    int b=key_target>>8, i=key_target&255;
    if(b!=frames[fsp].idx){
        if(fsp+1>=(int)(sizeof(frames)/sizeof(frames[0]))){
            tty_str("\r\n!! frame stack full"); return; }
        frames[++fsp].idx=b;
    }
    frames[fsp].i=i;
}

/* Исполнение слова: builtins делают свою работу и возвращают управление;
 * colon-слова (xt_colonref/xt_recur) пушат кадр — цикл run_top продолжит
 * исполнение с вершины стека кадров. Возврат из тела = pop кадра. */
static void exec_word(Word *w){
    int prev=fsp;
    g_jump=0;
    if(w->xt) w->xt(w);
    if(g_jump){ set_ip(g_jump_target); g_jump=0; return; }
    if(fsp==prev) frames[fsp].i++;   /* примитив: шаг вперёд */
    /* если fsp вырос (вызов слова) — ничего: ip вызывающего зафиксируем при pop */
}

static void run_top(void){
    for(;;){
        int fi=frames[fsp].i;
        if(fi>=BODY_MAX || !bodies[frames[fsp].idx][fi]){ /* конец тела -> возврат */
            if(fsp==0) break;
            fsp--;
            frames[fsp].i++;
            continue;
        }
        exec_word(bodies[frames[fsp].idx][fi]);
    }
}

/* точка входа colon-слов и рекурсии: просто push кадра */
static void xt_colonref(Word *w){ enter_body((int)w->param); } /* push кадра; run_top продолжит */

static void xt_recur(Word *w){ enter_body((int)w->param>>8); }

/* jump-примитивы: param = целевой индекс в ТЕКУЩЕМ теле */
static void xt_jmp (Word *w){ g_jump=1; g_jump_target=(int)w->param; }
static void xt_jmpf(Word *w){ if(pop()==0){ g_jump=1; g_jump_target=(int)w->param; } }
static void xt_jmpt(Word *w){ if(pop()!=0){ g_jump=1; g_jump_target=(int)w->param; } }

/* ============================ встроенные xt ============================ */
static void xt_lit(Word *w){ push(w->param); }
static void xt_plus (Word*){ cell b=pop(); push(pop()+b); }
static void xt_minus(Word*){ cell b=pop(); push(pop()-b); }
static void xt_mul  (Word*){ cell b=pop(); push(pop()*b); }
static void xt_div  (Word*){ cell b=pop(); if(!b){tty_str("\r\n!! div by zero");push(0);}else push(pop()/b); }
static void xt_mod  (Word*){ cell b=pop(); if(!b){tty_str("\r\n!! div by zero");push(0);}else push(pop()%b); }
static void xt_negate(Word*){ push(-pop()); }
static void xt_oneplus(Word*){ push(pop()+1); }
static void xt_oneminus(Word*){ push(pop()-1); }
static void xt_eq(Word*){ cell b=pop(); push(pop()==b?-1:0); }
static void xt_ne(Word*){ cell b=pop(); push(pop()!=b?-1:0); }
static void xt_lt(Word*){ cell b=pop(); push(pop()< b?-1:0); }
static void xt_gt(Word*){ cell b=pop(); push(pop()> b?-1:0); }
static void xt_le(Word*){ cell b=pop(); push(pop()<=b?-1:0); }
static void xt_ge(Word*){ cell b=pop(); push(pop()>=b?-1:0); }
static void xt_zeroeq(Word*){ push(pop()==0?-1:0); }
static void xt_and(Word*){ cell b=pop(); push(pop()&b); }
static void xt_or (Word*){ cell b=pop(); push(pop()|b); }
static void xt_dup (Word*){ push(peek(0)); }
static void xt_drop(Word*){ pop(); }
static void xt_swap(Word*){ cell a=pop(),b=pop(); push(a); push(b); }
static void xt_over(Word*){ push(peek(1)); }
static void xt_nip (Word*){ cell a=pop(); pop(); push(a); }
static void xt_tuck(Word*){ cell a=pop(),b=pop(); push(a); push(b); push(a); }
static void xt_rot (Word*){ cell c=pop(),b=pop(),a=pop(); push(b); push(c); push(a); }
static void xt_qdup(Word*){ if(peek(0)) push(peek(0)); }

static void print_num(cell v){
    char buf[12]; int i=0; ucell u;
    if(v<0){ emit('-'); u=(ucell)(-(v+1))+1u; } else u=(ucell)v;
    do{ buf[i++]=(char)('0'+u%10u); u/=10u; }while(u);
    while(i--) emit(buf[i]);
}
static void xt_dot(Word*){ print_num(pop()); emit(' '); }
static void xt_emit(Word*){ emit((char)pop()); }
static void xt_cr(Word*){ crlf(); }
static void xt_space(Word*){ emit(' '); }
static void xt_dot_s(Word*){
    tty_str("<");
    for(int i=0;i<=dsp;i++){ print_num(dstack[i]); emit(' '); }
    tty_str("top> ");
}
static void xt_store(Word*){ cell v=pop(); *(cell*)(uintptr_t)pop()=v; }
static void xt_fetch(Word*){ push(*(cell*)(uintptr_t)pop()); }
static void xt_addto(Word*){ cell *p=(cell*)(uintptr_t)pop(); *p+=pop(); }
static void xt_var_exec(Word *w){ push((cell)(uintptr_t)w->param); }
static void xt_const_exec(Word *w){ push(w->param); }
static void xt_dotq(Word *w){ tty_str((const char*)(uintptr_t)w->param); }
static void xt_printn(Word *w){ const cell *p=(const cell*)(uintptr_t)w->param;
    tty_out((const char*)(uintptr_t)p[0],(unsigned)p[1]); }

/* ============================ компилятор ============================ */
static int cur_body_idx=-1, cur_len;
static Word *jmp_stack[BODY_MAX]; static int jmp_sp=-1;
static int g_compile;
static int g_reset; /* флаг: ошибка прервала определение */

static void emit_compile(Word *w){
    if(!w) return;
    if(cur_body_idx<0 || cur_len>=BODY_MAX-1){
        tty_str("\r\n!! definition too long");
        g_compile=0; cur_body_idx=-1; g_reset=1; return;
    }
    bodies[cur_body_idx][cur_len++]=w;
}
static int here(void){ return cur_len; }
static int here_abs(void){ return (cur_body_idx<<8)|cur_len; }

static void abort_def(void); /* fwd (определение ниже, рядом с comp_colon) */

static Word *mk_anon(const char *tag, xt_t fn, cell p){
    Word *w=halloc(sizeof(Word));
    if(!w){ tty_str("\r\n!! oom"); return (Word*)0; }
    w->name=(char*)tag; w->nlen=(uint8_t)str_len(tag);
    w->xt=fn; w->param=p; w->immediate=0; w->next=(Word*)0;
    return w;
}

static void compile_number(cell v){ emit_compile(mk_anon("(LIT)",xt_lit,v)); }

/* ============================ парсер ============================ */
#define IN_BUF 128
static char inbuf[IN_BUF];
static int in_len,in_pos;

static void refill_line(void){
    in_pos=0; in_len=0;
    for(;;){
        int c=get_char_blocking();
        if(c=='\r'||c=='\n'){ emit('\n'); break; }
        if(c==8||c==127){ if(in_len){ in_len--; emit('\b'); emit(' '); emit('\b'); } continue; }
        if(in_len<IN_BUF-1){ inbuf[in_len++]=(char)c; emit((char)c); }
    }
    inbuf[in_len]=0;
}
static int token(char *out,int max){
    while(in_pos<in_len&&inbuf[in_pos]==' ') in_pos++;
    if(in_pos>=in_len) return 0;
    int n=0;
    while(in_pos<in_len&&inbuf[in_pos]!=' '&&n<max-1) out[n++]=inbuf[in_pos++];
    out[n]=0; return n;
}
static int is_number(const char *s,cell *v){
    int i=0,neg=0; long long acc=0;
    if(s[0]=='-'){ neg=1; i=1; }
    if(!s[i]) return 0;
    for(;s[i];i++){ if(s[i]<'0'||s[i]>'9') return 0;
        acc=acc*10+(s[i]-'0'); if(acc>2147483647LL) return 0; }
    *v=neg?(cell)(-acc):(cell)acc; return 1;
}

/* forward */
static void process_token(const char *tok,int n);

static void comp_colon(void);

static void abort_def(void){
    g_compile=0; cur_body_idx=-1; g_reset=1;
    while(jmp_sp>=0) jmp_sp--;
}

static void comp_colon(void){
    char tok[64], name[64];
    while(!token(tok,sizeof(tok))) refill_line();
    if(!str_len(tok)){ tty_str("\r\n!! name expected"); return; }
    if(next_body>=MAX_BODIES){ tty_str("\r\n!! too many definitions"); return; }
    if(find(tok,str_len(tok))){ tty_str("\r\n!! already defined: "); tty_str(tok); return; }
    unsigned nl=str_len(tok);
    for(unsigned i=0;i<=nl;i++) name[i]=tok[i];
    int idx=next_body;
    for(int i=0;i<BODY_MAX;i++) bodies[idx][i]=(Word*)0;
    cur_body_idx=idx; cur_len=0; g_compile=1; g_reset=0;
    for(;;){
        char t[64];
        while(!token(t,sizeof(t))){ refill_line(); }
        if(mem_eq(t,";",1)) break;
        process_token(t,(int)str_len(t));
        if(g_reset){ g_reset=0; return; }
    }
    g_compile=0;
    char *name_copy=halloc(nl+1);
    Word *w=(Word*)0;
    if(name_copy){
        for(unsigned i=0;i<=nl;i++) name_copy[i]=name[i];
        w=create(name_copy,nl);
    }
    if(w){ w->xt=xt_colonref; w->param=idx; next_body++; }
    cur_body_idx=-1;
    tty_str(" ok");
}

static void comp_variable(void){
    char tok[64];
    while(!token(tok,sizeof(tok))) refill_line();
    Word *w=create(tok,str_len(tok));
    if(!w) return;
    cell *mem=halloc(4);
    if(!mem){ tty_str("\r\n!! oom"); return; }
    *mem=0;
    w->xt=xt_var_exec; w->param=(cell)(uintptr_t)mem;
    tty_str(" ok");
}

static void comp_constant(void){
    char tok[64]; cell v=0;
    while(!token(tok,sizeof(tok))) refill_line();
    if(!is_number(tok,&v)){ tty_str("\r\n!! number expected"); return; }
    while(!token(tok,sizeof(tok))) refill_line();
    Word *w=create(tok,str_len(tok));
    if(w){ w->xt=xt_const_exec; w->param=v; tty_str(" ok"); }
}

static void comp_if(void){
    if(!g_compile){ tty_str("\r\n!! IF outside definition"); return; }
    Word *j=mk_anon("(JMPF)",xt_jmpf,0);
    if(!j) return;
    emit_compile(j);
    jmp_stack[++jmp_sp]=j;
}
static void push_marker(void){ jmp_stack[++jmp_sp]=(Word*)(uintptr_t)(intptr_t)here_abs(); }
static int pop_marker(void){ return (int)(intptr_t)jmp_stack[jmp_sp--]; }
static void comp_else(void){
    if(jmp_sp<0||cur_body_idx<0){ tty_str("\r\n!! ELSE without IF"); return; }
    Word *jf=jmp_stack[jmp_sp];
    Word *j=mk_anon("(JMP)",xt_jmp,0);
    if(!j) return;
    emit_compile(j);
    jf->param=here_abs();
    jmp_stack[jmp_sp]=j;
}
static void comp_then(void){
    if(jmp_sp<0||cur_body_idx<0){ tty_str("\r\n!! THEN without IF"); return; }
    jmp_stack[jmp_sp--]->param=here_abs();
}
static void comp_begin(void){
    if(!g_compile){ tty_str("\r\n!! BEGIN outside definition"); return; }
    push_marker();
}
static void comp_until(void){
    if(jmp_sp<0||cur_body_idx<0){ tty_str("\r\n!! UNTIL without BEGIN"); return; }
    int begin=pop_marker();
    emit_compile(mk_anon("(JMPF)",xt_jmpf,begin)); /* UNTIL: прыжок если FALSE */
}
static void comp_while(void){
    if(jmp_sp<0||cur_body_idx<0){ tty_str("\r\n!! WHILE without BEGIN"); return; }
    Word *j=mk_anon("(JMPF)",xt_jmpf,0);
    if(!j) return;
    emit_compile(j);
    jmp_stack[++jmp_sp]=j;
}
static void comp_repeat(void){
    if(jmp_sp<0||cur_body_idx<0){ tty_str("\r\n!! REPEAT without WHILE"); return; }
    Word *wf=jmp_stack[jmp_sp--];      /* WHILE-прыжок -> выход (сюда) */
    wf->param=here_abs();
    if(jmp_sp<0){ tty_str("\r\n!! REPEAT lost BEGIN"); return; }
    int begin=pop_marker();            /* маркер начала цикла */
    emit_compile(mk_anon("(JMP)",xt_jmp,begin));
}

/* ." / S" : читаем строку до '"' -> слово печати (." exec) или адрес+длина (S") */
static void comp_quote(int exec_print){
    char buf[IN_BUF]; int n=0; int closed=0;
    while(!closed){
        if(in_pos>=in_len) refill_line();
        while(in_pos<in_len&&!closed){
            char c=inbuf[in_pos++];
            if(c=='"'){ closed=1; break; }
            if(n<(int)sizeof(buf)-1) buf[n++]=c;
        }
    }
    buf[n]=0;
    char *s=halloc(n+1);
    if(!s){ tty_str("\r\n!! oom"); return; }
    for(int i=0;i<=n;i++) s[i]=buf[i];
    if(exec_print){
        Word *w=mk_anon("(DOTQ)",xt_dotq,(cell)(uintptr_t)s);
        if(w){ if(g_compile) emit_compile(w); else w->xt(w); }
    } else {
        /* S": кладём addr и len как два литерала; пользователь: S" abc" TYPE?
         * TYPE нет — используем (PRINTN): печатает len байт по addr со дна стека */
        cell *pair=halloc(8);
        if(!pair){ tty_str("\r\n!! oom"); return; }
        pair[0]=(cell)(uintptr_t)s; pair[1]=n;
        Word *w=mk_anon("(PRINTN)",xt_printn,(cell)(uintptr_t)pair);
        if(w){ if(g_compile) emit_compile(w); else w->xt(w); }
    }
}

/* единая обработка токена (exec или compile) */
static void process_token(const char *tok,int n){
    if(n==0) return;
    if(tok[0]=='('){ /* комментарий до ')' поперёк строк */
        int done=0;
        while(!done){
            while(in_pos<in_len&&!done) if(inbuf[in_pos++]==')') done=1;
            if(!done) refill_line();
        }
        return;
    }
    if(!g_compile && n==1 && tok[0]==':'){ comp_colon(); return; }
    if(g_compile && n==8 && mem_eq(tok,"VARIABLE",8)){ comp_variable(); return; }
    if(g_compile && n==8 && mem_eq(tok,"CONSTANT",8)){ comp_constant(); return; }
    if(g_compile && n==2 && mem_eq(tok,"IF",2)){ comp_if(); return; }
    if(g_compile && n==4 && mem_eq(tok,"ELSE",4)){ comp_else(); return; }
    if(g_compile && n==4 && mem_eq(tok,"THEN",4)){ comp_then(); return; }
    if(g_compile && n==5 && mem_eq(tok,"BEGIN",5)){ comp_begin(); return; }
    if(g_compile && n==5 && mem_eq(tok,"UNTIL",5)){ comp_until(); return; }
    if(g_compile && n==5 && mem_eq(tok,"WHILE",5)){ comp_while(); return; }
    if(g_compile && n==6 && mem_eq(tok,"REPEAT",6)){ comp_repeat(); return; }
    if(n==2 && tok[0]=='.' && tok[1]=='"'){ comp_quote(1); return; }
    if(n==2 && tok[0]=='S' && tok[1]=='"'){ comp_quote(0); return; }
    if(g_compile && n==3 && mem_eq(tok,"...",3)){ /* рекурсивный вызов текущего определения */
        emit_compile(mk_anon("(RECUR)",xt_recur,(cell)(cur_body_idx<<8)));
        return;
    }
    if(!g_compile && n==3 && mem_eq(tok,"BYE",3)){
        tty_str("\r\n[LibOS-Forth] bye");
        kernel_syscall(EX_SYS_EXIT,0,0,0);
        return;
    }

    Word *w=find(tok,(unsigned)n);
    if(w){
        if(g_compile && !w->immediate){ emit_compile(w); return; }
        if(w->xt==xt_colonref || w->xt==xt_recur){
            /* интерпретируемый вызов: кадр + полный прогон run_top */
            enter_body((int)w->param);
            run_top();
            return;
        }
        if(w->xt) w->xt(w);
        return;
    }
    cell num;
    if(is_number(tok,&num)){
        if(g_compile) compile_number(num); else push(num);
        return;
    }
    tty_str("\r\n?? unknown word: "); tty_out(tok,(unsigned)n);
    if(g_compile) abort_def();
}

/* публичный вход для скриптов/REPL */
void forth_eval_line(void){
    char tok[64]; int n;
    while((n=token(tok,sizeof(tok)))!=0)
        process_token(tok,n);
}

/* ============================ инициализация ============================ */
static void init_dict(void){
    add_builtin("+",xt_plus,0);     add_builtin("-",xt_minus,0);
    add_builtin("*",xt_mul,0);      add_builtin("/",xt_div,0);
    add_builtin("MOD",xt_mod,0);    add_builtin("NEGATE",xt_negate,0);
    add_builtin("1+",xt_oneplus,0); add_builtin("1-",xt_oneminus,0);
    add_builtin("=",xt_eq,0);       add_builtin("<>",xt_ne,0);
    add_builtin("<",xt_lt,0);       add_builtin(">",xt_gt,0);
    add_builtin("<=",xt_le,0);      add_builtin(">=",xt_ge,0);
    add_builtin("0=",xt_zeroeq,0);
    add_builtin("AND",xt_and,0);    add_builtin("OR",xt_or,0);
    add_builtin("DUP",xt_dup,0);    add_builtin("DROP",xt_drop,0);
    add_builtin("SWAP",xt_swap,0);  add_builtin("OVER",xt_over,0);
    add_builtin("NIP",xt_nip,0);    add_builtin("TUCK",xt_tuck,0);
    add_builtin("ROT",xt_rot,0);    add_builtin("?DUP",xt_qdup,0);
    add_builtin(".",xt_dot,0);      add_builtin("EMIT",xt_emit,0);
    add_builtin("CR",xt_cr,0);      add_builtin("SPACE",xt_space,0);
    add_builtin(".S",xt_dot_s,0);
    add_builtin("!",xt_store,0);    add_builtin("@",xt_fetch,0);
    add_builtin("+!",xt_addto,0);
}

/* REPL-точка входа LibOS (ядро вызывает её как init-LibOS на M1) */
void forth_main(void){
    init_dict();
    crlf();
    tty_str("=== LispOS LibOS-Forth v0.1 (M1) ===\r\n");
    for(;;){
        tty_str("\r\nFORTH> ");
        refill_line();
        g_compile=0; jmp_sp=-1; g_reset=0;
        forth_eval_line();
    }
}
