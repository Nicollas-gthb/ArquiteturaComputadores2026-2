/* poc.c - PoC: Arquiteturas Superescalares e ILP
 *
 * Compilar (MinGW 32 bits):  gcc -O1 -msse2 -mfpmath=sse -o poc.exe poc.c
 * Executar:                  poc.exe 1 | 2 | 3 | 4 | 5 | all
 *
 * Experimento 1: varias unidades funcionais ao mesmo tempo (IPC > 1)   -> secao 2.2
 * Experimento 2: dependencia verdadeira (RAW) em codigo C              -> secao 2.3
 * Experimento 3: execucao fora de ordem (o exemplo I1..I4)             -> secoes 2.3 e 2.4
 * Experimento 4: renomeacao de registradores (WAW/WAR)                 -> secao 2.4
 * Experimento 5: previsao de desvios e descarte de especulacao         -> secao 2.4
 */
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
static double agora(void) {                       /* tempo em segundos */
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)f.QuadPart;
}
static void fixa_nucleo(void) {                   /* P-core (logico 0) + prioridade alta */
    SetProcessAffinityMask(GetCurrentProcess(), 1);
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
}
#else
#include <time.h>
static double agora(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}
static void fixa_nucleo(void) {}
#endif

#define NOINLINE __attribute__((noinline))

/* Repete a medicao n vezes e fica com o MENOR tempo (o que sofreu menos
 * interferencia do sistema e da variacao de frequencia). */
#define MELHOR(n, expr) ({ double _m = 1e30;                              \
    for (int _i = 0; _i < (n); _i++) { double _t = (expr); if (_t < _m) _m = _t; } \
    _m; })

/* ====================================================================
 * EXPERIMENTO 1 - Varias cadeias independentes de ADD (secao 2.2)
 * ==================================================================== */
/* ADD registrador+registrador: r = r + esi (esi vale 1).
 * Nao usamos "add $1, reg": os nucleos P do Alder Lake resolvem somas com
 * constante pequena no proprio renomeador, sem usar a ALU, e isso esconderia
 * a dependencia que queremos medir (ver secao 4.12). */
#define ADDR(r) "addl %%esi, %%" r "\n\t"
#define R4(x)   x x x x

/* Cada macro Kn tem 12 ADDs distribuidos em n registradores diferentes */
#define K1  R4(ADDR("eax")) R4(ADDR("eax")) R4(ADDR("eax"))
#define P2  ADDR("eax") ADDR("ebx")
#define K2  R4(P2) P2 P2
#define P3  ADDR("eax") ADDR("ebx") ADDR("edx")
#define K3  R4(P3)
#define P4  ADDR("eax") ADDR("ebx") ADDR("edx") ADDR("edi")
#define K4  P4 P4 P4

/* Laco escrito em Assembly: 12 ADDs + DEC + JNZ = 14 instrucoes por volta */
#define CADEIA(nome, CORPO)                                              \
static NOINLINE double nome(unsigned n) {                                \
    double t0 = agora();                                                 \
    __asm__ volatile(                                                    \
        "movl $1,%%esi\n\t"                                              \
        "xorl %%eax,%%eax\n\t" "xorl %%ebx,%%ebx\n\t"                    \
        "xorl %%edx,%%edx\n\t" "xorl %%edi,%%edi\n\t"                    \
        "1:\n\t"                                                         \
        CORPO                                                            \
        "decl %0\n\t" "jnz 1b\n\t"                                       \
        : "+c"(n) : : "eax", "ebx", "edx", "esi", "edi", "cc");          \
    return agora() - t0;                                                 \
}
CADEIA(cadeia1, K1)    /* 1 cadeia : cada ADD depende do anterior (RAW)  */
CADEIA(cadeia2, K2)    /* 2 cadeias independentes                         */
CADEIA(cadeia3, K3)    /* 3 cadeias independentes                         */
CADEIA(cadeia4, K4)    /* 4 cadeias independentes                         */

/* Estima a frequencia real (GHz): na cadeia1, cada ADD depende do anterior e
 * o ADD registrador+registrador tem latencia de 1 ciclo, logo 1 ADD = 1 ciclo. */
static double freq(void) {
    const unsigned it = 30000000u;
    cadeia1(it);                                  /* aquecimento (turbo) */
    double t = MELHOR(3, cadeia1(it));
    double ghz = (12.0 * it) / t / 1e9;
    if (ghz > 5.0 || ghz < 0.8)
        printf("AVISO: frequencia estimada (%.2f GHz) fora do plausivel; ciclos/IPC nao confiaveis\n", ghz);
    return ghz;
}

static void experimento1(void) {
    const unsigned IT = 100000000u;               /* 1e8 voltas x 12 ADDs */
    double ghz = freq();
    double t[5];
    t[1] = MELHOR(5, cadeia1(IT)); t[2] = MELHOR(5, cadeia2(IT));
    t[3] = MELHOR(5, cadeia3(IT)); t[4] = MELHOR(5, cadeia4(IT));
    printf("[Exp 1] freq_est = %.2f GHz (1 ADD dependente = 1 ciclo)\n", ghz);
    printf("cadeias | tempo(s) | ciclos/volta | ADDs/ciclo | IPC_est\n");
    for (int k = 1; k <= 4; k++) {
        double cv = t[k] * ghz * 1e9 / IT;        /* ciclos por volta do laco */
        printf("   %d    | %7.3f  |   %7.2f    |   %5.2f    | %5.2f\n",
               k, t[k], cv, 12.0 / cv, 14.0 / cv);
    }
}

/* ====================================================================
 * EXPERIMENTO 2 - Dependencia verdadeira (RAW) em C (secao 2.3)
 * ==================================================================== */
#define N    2048                                 /* 16 KB de doubles: cabe na L1d */
#define REPS 100000
/* 1 acumulador: s += v[i] depende do s da iteracao anterior (RAW) */
static NOINLINE double soma1(const double *v) {
    double s = 0.0;
    for (int i = 0; i < N; i++)
        s += v[i];
    return s;
}
/* 4 acumuladores: quatro cadeias RAW independentes entre si */
static NOINLINE double soma4(const double *v) {
    double s0 = 0, s1 = 0, s2 = 0, s3 = 0;
    for (int i = 0; i < N; i += 4) {
        s0 += v[i]; s1 += v[i + 1]; s2 += v[i + 2]; s3 += v[i + 3];
    }
    return (s0 + s1) + (s2 + s3);
}
/* 8 acumuladores: oito cadeias RAW independentes entre si */
static NOINLINE double soma8(const double *v) {
    double s0 = 0, s1 = 0, s2 = 0, s3 = 0, s4 = 0, s5 = 0, s6 = 0, s7 = 0;
    for (int i = 0; i < N; i += 8) {
        s0 += v[i];     s1 += v[i + 1]; s2 += v[i + 2]; s3 += v[i + 3];
        s4 += v[i + 4]; s5 += v[i + 5]; s6 += v[i + 6]; s7 += v[i + 7];
    }
    return ((s0 + s1) + (s2 + s3)) + ((s4 + s5) + (s6 + s7));
}

static volatile double sink2;
static double roda_soma(double (*f)(const double *), const double *v) {
    double t0 = agora();
    for (int r = 0; r < REPS; r++) sink2 += f(v);
    return agora() - t0;
}

static void experimento2(void) {
    static double v[N];
    for (int i = 0; i < N; i++) v[i] = 1.0;
    double (*f[3])(const double *) = { soma1, soma4, soma8 };
    const char *nome[3] = { "1 acumulador ", "4 acumuladores", "8 acumuladores" };
    double ghz = freq();
    printf("[Exp 2] freq_est = %.2f GHz\n", ghz);
    printf("        variante       | tempo(s) | ns/soma | ciclos/soma\n");
    for (int k = 0; k < 3; k++) {
        double t = MELHOR(5, roda_soma(f[k], v));
        double somas = (double)N * REPS;
        printf("        %s | %7.3f  | %7.3f |   %6.3f\n",
               nome[k], t, t / somas * 1e9, t * ghz * 1e9 / somas);
    }
}

/* ====================================================================
 * EXPERIMENTO 3 - Execucao fora de ordem: o exemplo I1..I4 (secoes 2.3/2.4)
 * ==================================================================== */
/* I1/I2: ADDSD com RAW em xmm0 (latencia de varios ciclos por volta).
 * FILL: instrucoes independentes (I3, I4...) que nao dependem de xmm0. */
#define FIL4 ADDR("eax") ADDR("ebx") ADDR("edx") ADDR("edi")
static double um = 1.0;
#define OOO(nome, FILL)                                                  \
static NOINLINE double nome(unsigned n) {                                \
    double t0 = agora();                                                 \
    __asm__ volatile(                                                    \
        "movsd %1, %%xmm1\n\t"          /* xmm1 = 1.0                  */\
        "xorpd %%xmm0,%%xmm0\n\t"                                        \
        "movl $1,%%esi\n\t"                                              \
        "xorl %%eax,%%eax\n\t" "xorl %%ebx,%%ebx\n\t"                    \
        "xorl %%edx,%%edx\n\t" "xorl %%edi,%%edi\n\t"                    \
        "1:\n\t"                                                         \
        "addsd %%xmm1, %%xmm0\n\t"      /* cadeia RAW (I1 -> I2 ...)   */\
        FILL                              /* trabalho independente     */\
        "decl %0\n\t" "jnz 1b\n\t"                                       \
        : "+c"(n) : "m"(um)                                              \
        : "xmm0", "xmm1", "eax", "ebx", "edx", "esi", "edi", "cc");      \
    return agora() - t0;                                                 \
}
OOO(ooo0, "")              /* so a cadeia dependente: 3 instrucoes por volta */
OOO(ooo4, FIL4)            /* + 4 instrucoes independentes: 7 por volta      */
OOO(ooo8, FIL4 FIL4)       /* + 8 instrucoes independentes: 11 por volta     */

static void experimento3(void) {
    const unsigned IT = 300000000u;
    double ghz = freq();
    double (*f[3])(unsigned) = { ooo0, ooo4, ooo8 };
    const int instr[3] = { 3, 7, 11 }, extra[3] = { 0, 4, 8 };
    printf("[Exp 3] freq_est = %.2f GHz\n", ghz);
    printf("independentes | instr/volta | tempo(s) | ciclos/volta | IPC_est\n");
    for (int k = 0; k < 3; k++) {
        double t = MELHOR(5, f[k](IT));
        double cv = t * ghz * 1e9 / IT;
        printf("      %2d      |     %2d      | %7.3f  |    %6.2f    | %5.2f\n",
               extra[k], instr[k], t, cv, instr[k] / cv);
    }
}

/* ====================================================================
 * EXPERIMENTO 4 - Renomeacao de registradores (secao 2.4)
 * ==================================================================== */
/* Par IMUL + ADD: o IMUL ESCREVE no temporario t sem le-lo (nao ha RAW
 * entre voltas), mas reusar o mesmo nome cria WAW (IMUL x IMUL) e WAR
 * (ADD le t, o proximo IMUL escreve t). So a renomeacao remove isso. */
#define PAR(t)  "imull $3, %%esi, %%" t "\n\t"  "addl %%" t ", %%edx\n\t"
#define MESMO   PAR("eax") PAR("eax") PAR("eax") PAR("eax") \
                PAR("eax") PAR("eax") PAR("eax") PAR("eax")
#define DISTINTO PAR("eax") PAR("ebx") PAR("edi") PAR("eax") \
                 PAR("ebx") PAR("edi") PAR("eax") PAR("ebx")
#define REN(nome, CORPO)                                                 \
static NOINLINE double nome(unsigned n) {                                \
    double t0 = agora();                                                 \
    __asm__ volatile(                                                    \
        "movl $5, %%esi\n\t" "xorl %%edx,%%edx\n\t"                      \
        "1:\n\t"                                                         \
        CORPO                                                            \
        "decl %0\n\t" "jnz 1b\n\t"                                       \
        : "+c"(n) : : "eax", "ebx", "edx", "esi", "edi", "cc");          \
    return agora() - t0;                                                 \
}
REN(ren_mesmo, MESMO)       /* 8 pares reusando o MESMO registrador (eax)  */
REN(ren_distinto, DISTINTO) /* 8 pares alternando eax, ebx, edi            */

static void experimento4(void) {
    const unsigned IT = 100000000u;               /* 1e8 voltas x 8 pares */
    double ghz = freq();
    double t1 = 1e30, t2 = 1e30;
    for (int i = 0; i < 5; i++) {                 /* intercala as duas versoes */
        double a = ren_mesmo(IT), b = ren_distinto(IT);
        if (a < t1) t1 = a;
        if (b < t2) t2 = b;
    }
    printf("[Exp 4] freq_est = %.2f GHz\n", ghz);
    printf("nomes de registrador | tempo(s) | ciclos/par | IPC_est\n");
    printf("   1 (so eax)        | %7.3f  |  %6.3f    | %5.2f\n",
           t1, t1 * ghz * 1e9 / IT / 8, 18.0 / (t1 * ghz * 1e9 / IT));
    printf("   3 (eax,ebx,edi)   | %7.3f  |  %6.3f    | %5.2f\n",
           t2, t2 * ghz * 1e9 / IT / 8, 18.0 / (t2 * ghz * 1e9 / IT));
    printf("referencia: sem renomeacao, IMUL (3 ciclos) e ADD teriam que ser serializados\n");
}

/* ====================================================================
 * EXPERIMENTO 5 - Previsao de desvios e especulacao (secao 2.4)
 * ==================================================================== */
#define NB    32768
#define REPSB 10000
/* O if abaixo vira um DESVIO CONDICIONAL (jl/jge). O asm volatile vazio
 * impede o compilador de trocar o desvio por CMOV (sem desvio, sem teste). */
static NOINLINE long conta(const int *v) {
    long s = 0;
    for (int i = 0; i < NB; i++)
        if (v[i] >= 128) {
            s += v[i];
            __asm__ volatile("");
        }
    return s;
}

static int cmp(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

static volatile long sink5;
static double roda_conta(const int *v) {
    double t0 = agora();
    for (int r = 0; r < REPSB; r++) sink5 += conta(v);
    return agora() - t0;
}

static void experimento5(void) {
    static int v[NB];
    srand(42);
    for (int i = 0; i < NB; i++) v[i] = rand() % 256;
    double ghz = freq();
    double t_des = MELHOR(3, roda_conta(v));
    qsort(v, NB, sizeof(int), cmp);
    double t_ord = MELHOR(3, roda_conta(v));
    double elems = (double)NB * REPSB;
    double erros = 0.5 * elems;                   /* dado aleatorio 50/50: ~50% de erros */
    printf("[Exp 5] freq_est = %.2f GHz\n", ghz);
    printf("vetor      | tempo(s) | ns/elemento | ciclos/elemento\n");
    printf("desordenado| %7.3f  |   %6.3f    |   %6.2f\n", t_des, t_des / elems * 1e9, t_des * ghz * 1e9 / elems);
    printf("ordenado   | %7.3f  |   %6.3f    |   %6.2f\n", t_ord, t_ord / elems * 1e9, t_ord * ghz * 1e9 / elems);
    printf("razao desordenado/ordenado: %.1fx\n", t_des / t_ord);
    printf("penalidade estimada por erro de previsao: %.1f ciclos (assumindo ~50%% de erros)\n",
           (t_des - t_ord) * ghz * 1e9 / erros);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "uso: %s 1|2|3|4|5|all\n", argv[0]); return 1; }
    fixa_nucleo();
    int all = (argv[1][0] == 'a');
    int e = atoi(argv[1]);
    if (all || e == 1) experimento1();
    if (all || e == 2) experimento2();
    if (all || e == 3) experimento3();
    if (all || e == 4) experimento4();
    if (all || e == 5) experimento5();
    return 0;
}