#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>

// --- Configurações da Simulação (facilmente ajustáveis) ---
#define TEMPO_DE_SIMULACAO 1
#define NUM_PISTAS 3
#define NUM_PORTOES 5
#define TORRE_CAPACIDADE 2
#define STARVATION_ALERT_SECONDS 10  // Tempo reduzido para demonstração
#define STARVATION_CRASH_SECONDS 15  // Tempo reduzido para demonstração
#define MAX_VOOS 200 // Número máximo de voos para a simulação

// --- Enums para Clareza ---
typedef enum { DOMESTICO, INTERNACIONAL } TipoVoo;
typedef enum { AGENDADO, POUSANDO, DESEMBARCANDO, AGUARDANDO_DECOLAGEM, DECOLANDO, CONCLUIDO, ACIDENTE, DEADLOCK } StatusVoo;

// --- Estruturas de Dados ---

// Informações do Voo
typedef struct {
    int id;
    TipoVoo tipo;
    StatusVoo status;
    pthread_t thread_id;
    time_t tempo_espera_inicio;
} InfoVoo;

// Recursos do Aeroporto
typedef struct {
    // Pistas
    int pistas_disponiveis;
    pthread_mutex_t mutex_pista;
    pthread_cond_t cond_pista;
    int esperando_pista_intl;

    // Portões
    int portoes_disponiveis;
    pthread_mutex_t mutex_portao;
    pthread_cond_t cond_portao;
    int esperando_portao_intl;

    // Torre de Controle
    int usuarios_torre;
    pthread_mutex_t mutex_torre;
    pthread_cond_t cond_torre_intl;
    pthread_cond_t cond_torre_dom;
    int esperando_torre_intl;
} RecursosAeroporto;

// Estatísticas da Simulação
typedef struct {
    int voos_sucesso;
    int voos_acidentados;
    int incidentes_deadlock;
    int alertas_starvation;
    pthread_mutex_t mutex_stats;
} StatsSimulacao;

// --- Variáveis Globais ---
RecursosAeroporto aeroporto;
StatsSimulacao stats;
volatile int simulacao_rodando = 1;
InfoVoo todos_voos[MAX_VOOS];
int total_voos_criados = 0;
pthread_mutex_t mutex_log = PTHREAD_MUTEX_INITIALIZER;

// --- Protótipos das Funções ---
void inicializar_aeroporto();
void destruir_aeroporto();
void* ciclo_vida_aviao(void *arg);
void imprimir_relatorio_final();

// Funções de Log e Utilidades
const char* get_tipo_str(TipoVoo tipo) { return tipo == INTERNACIONAL ? "Internacional" : "Domestico"; }
const char* get_status_str(StatusVoo status);
void log_evento(int id, TipoVoo tipo, const char* mensagem);

// Funções de Gerenciamento de Recursos
int solicitar_recurso(InfoVoo *voo, pthread_mutex_t *mutex, pthread_cond_t *cond_intl, pthread_cond_t *cond_dom, int *recurso_disponivel, int *esperando_intl);
void liberar_recurso(pthread_mutex_t *mutex, pthread_cond_t *cond_intl, pthread_cond_t *cond_dom, int *recurso_disponivel, int *esperando_intl);
int solicitar_torre(InfoVoo *voo);
void liberar_torre(InfoVoo *voo);

// --- Função Principal ---
int main() {
    srand(time(NULL));
    inicializar_aeroporto();

    printf("--- Simulacao de Controle de Trafego Aereo Iniciada (Duracao: %d min) ---\n", TEMPO_DE_SIMULACAO);
    printf("Pistas: %d, Portoes: %d, Capacidade da Torre: %d\n\n", NUM_PISTAS, NUM_PORTOES, TORRE_CAPACIDADE);

    time_t inicio_simulacao = time(NULL);
    
    pthread_t threads[MAX_VOOS];

    while (time(NULL) - inicio_simulacao < TEMPO_DE_SIMULACAO * 60) {
        if (total_voos_criados < MAX_VOOS) {
            InfoVoo* info = &todos_voos[total_voos_criados];
            info->id = total_voos_criados + 1;
            info->tipo = (rand() % 3 == 0) ? INTERNACIONAL : DOMESTICO; // Mais voos domésticos
            info->status = AGENDADO;

            pthread_create(&threads[total_voos_criados], NULL, ciclo_vida_aviao, (void*)info);
            total_voos_criados++;
        }
        sleep(rand() % 3 + 1); // Cria um novo voo a cada 1-3 segundos
    }

    printf("\n--- TEMPO DE SIMULACAO ESGOTADO ---\n");
    printf("Nenhum novo voo será criado. Aguardando operações em andamento...\n");
    simulacao_rodando = 0;

    for (int i = 0; i < total_voos_criados; i++) {
        pthread_join(threads[i], NULL);
    }
    
    imprimir_relatorio_final();
    destruir_aeroporto();

    return 0;
}

// --- Implementação do Ciclo de Vida do Avião ---
void* ciclo_vida_aviao(void* arg) {
    InfoVoo *voo = (InfoVoo*)arg;
    char msg[200];

    // 1. POUSO
    voo->status = POUSANDO;
    sprintf(msg, "iniciando procedimento de pouso.");
    log_evento(voo->id, voo->tipo, msg);

    if (voo->tipo == INTERNACIONAL) {
        if (!solicitar_recurso(voo, &aeroporto.mutex_pista, NULL, NULL, &aeroporto.pistas_disponiveis, &aeroporto.esperando_pista_intl)) return NULL;
        if (!solicitar_torre(voo)) { liberar_recurso(&aeroporto.mutex_pista, NULL, NULL, &aeroporto.pistas_disponiveis, NULL); return NULL; }
    } else { // DOMESTICO
        if (!solicitar_torre(voo)) return NULL;
        if (!solicitar_recurso(voo, &aeroporto.mutex_pista, NULL, NULL, &aeroporto.pistas_disponiveis, &aeroporto.esperando_pista_intl)) { liberar_torre(voo); return NULL; }
    }
    
    log_evento(voo->id, voo->tipo, "pista e torre alocadas. Pousando...");
    sleep(3); // Simula o tempo de pouso
    log_evento(voo->id, voo->tipo, "pouso concluido. Liberando pista.");

    liberar_recurso(&aeroporto.mutex_pista, NULL, NULL, &aeroporto.pistas_disponiveis, &aeroporto.esperando_pista_intl);
    liberar_torre(voo); // Torre liberada após pouso, mas solicitada novamente para desembarque

    // 2. DESEMBARQUE
    voo->status = DESEMBARCANDO;
    log_evento(voo->id, voo->tipo, "solicitando portao e torre para desembarque.");

    if (!solicitar_recurso(voo, &aeroporto.mutex_portao, NULL, NULL, &aeroporto.portoes_disponiveis, &aeroporto.esperando_portao_intl)) return NULL;
    if (!solicitar_torre(voo)) { liberar_recurso(&aeroporto.mutex_portao, NULL, NULL, &aeroporto.portoes_disponiveis, NULL); return NULL; }

    log_evento(voo->id, voo->tipo, "portao e torre alocados. Desembarcando passageiros...");
    liberar_torre(voo); // Torre liberada assim que a comunicação de desembarque termina
    sleep(5); // Simula o tempo de desembarque
    log_evento(voo->id, voo->tipo, "desembarque concluido. Aguardando para decolagem.");
    
    // 3. DECOLAGEM
    voo->status = AGUARDANDO_DECOLAGEM;
    sleep(rand() % 10 + 5); // Tempo em solo

    voo->status = DECOLANDO;
    log_evento(voo->id, voo->tipo, "iniciando procedimento de decolagem.");

    if (voo->tipo == INTERNACIONAL) {
        // Portão já está alocado. Solicita pista e torre.
        if (!solicitar_recurso(voo, &aeroporto.mutex_pista, NULL, NULL, &aeroporto.pistas_disponiveis, &aeroporto.esperando_pista_intl)) { liberar_recurso(&aeroporto.mutex_portao, NULL, NULL, &aeroporto.portoes_disponiveis, NULL); return NULL; }
        if (!solicitar_torre(voo)) { 
            liberar_recurso(&aeroporto.mutex_portao, NULL, NULL, &aeroporto.portoes_disponiveis, NULL); 
            liberar_recurso(&aeroporto.mutex_pista, NULL, NULL, &aeroporto.pistas_disponiveis, NULL); 
            return NULL; 
        }
    } else { // DOMESTICO
        if (!solicitar_torre(voo)) { liberar_recurso(&aeroporto.mutex_portao, NULL, NULL, &aeroporto.portoes_disponiveis, NULL); return NULL; }
        if (!solicitar_recurso(voo, &aeroporto.mutex_pista, NULL, NULL, &aeroporto.pistas_disponiveis, &aeroporto.esperando_pista_intl)) { 
            liberar_recurso(&aeroporto.mutex_portao, NULL, NULL, &aeroporto.portoes_disponiveis, NULL);
            liberar_torre(voo); 
            return NULL; 
        }
    }

    log_evento(voo->id, voo->tipo, "portao, pista e torre alocados. Decolando...");
    sleep(3);
    log_evento(voo->id, voo->tipo, "decolagem concluida. Liberando todos os recursos.");

    liberar_recurso(&aeroporto.mutex_portao, NULL, NULL, &aeroporto.portoes_disponiveis, &aeroporto.esperando_portao_intl);
    liberar_recurso(&aeroporto.mutex_pista, NULL, NULL, &aeroporto.pistas_disponiveis, &aeroporto.esperando_pista_intl);
    liberar_torre(voo);
    
    voo->status = CONCLUIDO;
    log_evento(voo->id, voo->tipo, "ciclo de operacoes finalizado com sucesso.");
    pthread_mutex_lock(&stats.mutex_stats);
    stats.voos_sucesso++;
    pthread_mutex_unlock(&stats.mutex_stats);

    return NULL;
}

// --- Funções de Gerenciamento de Recursos ---

int solicitar_torre(InfoVoo *voo) {
    char msg[100];
    sprintf(msg, "solicitando acesso a torre.");
    log_evento(voo->id, voo->tipo, msg);

    pthread_mutex_lock(&aeroporto.mutex_torre);

    if (voo->tipo == INTERNACIONAL) {
        aeroporto.esperando_torre_intl++;
        while (aeroporto.usuarios_torre >= TORRE_CAPACIDADE) {
            pthread_cond_wait(&aeroporto.cond_torre_intl, &aeroporto.mutex_torre);
        }
        aeroporto.esperando_torre_intl--;
    } else { // DOMESTICO
        voo->tempo_espera_inicio = time(NULL);
        while (aeroporto.usuarios_torre >= TORRE_CAPACIDADE || aeroporto.esperando_torre_intl > 0) { // Espera se a torre está cheia OU outro avião internacional na fila
            struct timespec timeout;
            clock_gettime(CLOCK_REALTIME, &timeout);
            timeout.tv_sec += 1; // Verifica a cada segundo

            int rc = pthread_cond_timedwait(&aeroporto.cond_torre_dom, &aeroporto.mutex_torre, &timeout);
            
            if (rc == ETIMEDOUT) {
                time_t agora = time(NULL);
                int tempo_espera = agora - voo->tempo_espera_inicio;

                if (tempo_espera >= STARVATION_CRASH_SECONDS) {
                    log_evento(voo->id, voo->tipo, "CRASH! Tempo de espera pela torre excedeu o limite. Falha operacional.");
                    voo->status = ACIDENTE;
                    pthread_mutex_lock(&stats.mutex_stats);
                    stats.voos_acidentados++;
                    pthread_mutex_unlock(&stats.mutex_stats);
                    pthread_mutex_unlock(&aeroporto.mutex_torre);
                    return 0; // FALHA
                } else if (tempo_espera >= STARVATION_ALERT_SECONDS && tempo_espera < STARVATION_CRASH_SECONDS) {
                    log_evento(voo->id, voo->tipo, "ALERTA CRITICO! Risco de starvation, tempo de espera elevado.");
                    pthread_mutex_lock(&stats.mutex_stats);
                    stats.alertas_starvation++;
                    pthread_mutex_unlock(&stats.mutex_stats);
                }
            }
        }
    }
    
    aeroporto.usuarios_torre++;
    pthread_mutex_unlock(&aeroporto.mutex_torre);
    return 1; // SUCESSO
}


void liberar_torre(InfoVoo *voo) {
    pthread_mutex_lock(&aeroporto.mutex_torre);
    aeroporto.usuarios_torre--;
    // Prioriza voos internacionais
    if (aeroporto.esperando_torre_intl > 0) {
        pthread_cond_broadcast(&aeroporto.cond_torre_intl);
    } else {
        pthread_cond_broadcast(&aeroporto.cond_torre_dom);
    }
    pthread_mutex_unlock(&aeroporto.mutex_torre);
}


int solicitar_recurso(InfoVoo *voo, pthread_mutex_t *mutex, pthread_cond_t *cond_intl, pthread_cond_t *cond_dom, int *recurso_disponivel, int *esperando_intl) {
    pthread_mutex_lock(mutex);
    while (*recurso_disponivel == 0) {
        // Lógica de espera simples para pistas e portões
        // Em um sistema real, aqui também haveria prioridade
        pthread_cond_wait(&aeroporto.cond_pista, mutex); // Usando cond_pista para ambos por simplicidade
    }
    (*recurso_disponivel)--;
    pthread_mutex_unlock(mutex);
    return 1;
}

void liberar_recurso(pthread_mutex_t *mutex, pthread_cond_t *cond_intl, pthread_cond_t *cond_dom, int *recurso_disponivel, int *esperando_intl) {
    pthread_mutex_lock(mutex);
    (*recurso_disponivel)++;
    pthread_cond_broadcast(&aeroporto.cond_pista); // Acorda todos que esperam
    pthread_mutex_unlock(mutex);
}


// --- Funções de Inicialização e Finalização ---

void inicializar_aeroporto() {
    // Recursos
    aeroporto.pistas_disponiveis = NUM_PISTAS;
    aeroporto.portoes_disponiveis = NUM_PORTOES;
    aeroporto.usuarios_torre = 0;
    aeroporto.esperando_pista_intl = 0;
    aeroporto.esperando_portao_intl = 0;
    aeroporto.esperando_torre_intl = 0;

    // Mutexes
    pthread_mutex_init(&aeroporto.mutex_pista, NULL);
    pthread_mutex_init(&aeroporto.mutex_portao, NULL);
    pthread_mutex_init(&aeroporto.mutex_torre, NULL);
    pthread_mutex_init(&stats.mutex_stats, NULL);

    // Variáveis de Condição
    pthread_cond_init(&aeroporto.cond_pista, NULL);
    pthread_cond_init(&aeroporto.cond_portao, NULL);
    pthread_cond_init(&aeroporto.cond_torre_intl, NULL);
    pthread_cond_init(&aeroporto.cond_torre_dom, NULL);

    // Estatísticas
    stats.voos_sucesso = 0;
    stats.voos_acidentados = 0;
    stats.incidentes_deadlock = 0; // A detecção de deadlock não foi implementada explicitamente, mas a estrutura está aqui.
    stats.alertas_starvation = 0;
}

void destruir_aeroporto() {
    pthread_mutex_destroy(&aeroporto.mutex_pista);
    pthread_mutex_destroy(&aeroporto.mutex_portao);
    pthread_mutex_destroy(&aeroporto.mutex_torre);
    pthread_mutex_destroy(&stats.mutex_stats);
    pthread_mutex_destroy(&mutex_log);

    pthread_cond_destroy(&aeroporto.cond_pista);
    pthread_cond_destroy(&aeroporto.cond_portao);
    pthread_cond_destroy(&aeroporto.cond_torre_intl);
    pthread_cond_destroy(&aeroporto.cond_torre_dom);
}


// --- Funções de Relatório e Log ---

void log_evento(int id, TipoVoo tipo, const char* mensagem) {
    time_t agora = time(NULL);
    struct tm * ptm = localtime(&agora);
    char buf[30];
    strftime(buf, 30, "%H:%M:%S", ptm);
    
    pthread_mutex_lock(&mutex_log);
    printf("[%s] Voo %03d (%s): %s\n", buf, id, get_tipo_str(tipo), mensagem);
    fflush(stdout);
    pthread_mutex_unlock(&mutex_log);
}

const char* get_status_str(StatusVoo status) {
    switch (status) {
        case AGENDADO: return "Agendado";
        case POUSANDO: return "Pousando";
        case DESEMBARCANDO: return "Desembarcando";
        case AGUARDANDO_DECOLAGEM: return "Aguardando Decolagem";
        case DECOLANDO: return "Decolando";
        case CONCLUIDO: return "Concluido";
        case ACIDENTE: return "ACIDENTE (Starvation)";
        case DEADLOCK: return "DEADLOCK";
        default: return "Desconhecido";
    }
}

void imprimir_relatorio_final() {
    printf("\n\n======================================================\n");
    printf("              RELATORIO FINAL DA SIMULACAO\n");
    printf("======================================================\n");

    printf("\n--- METRICAS GERAIS ---\n");
    printf("Total de voos criados: %d\n", total_voos_criados);
    printf("Voos concluidos com sucesso: %d\n", stats.voos_sucesso);
    printf("Voos acidentados (starvation): %d\n", stats.voos_acidentados);
    printf("Alertas de starvation emitidos: %d\n", stats.alertas_starvation);
    printf("Incidentes de deadlock detectados: %d\n", stats.incidentes_deadlock);

    printf("\n--- ESTADO FINAL DOS VOOS ---\n");
    for (int i = 0; i < total_voos_criados; i++) {
        InfoVoo *voo = &todos_voos[i];
        printf("Voo %03d (%-13s) - Status Final: %s\n", voo->id, get_tipo_str(voo->tipo), get_status_str(voo->status));
    }
    printf("\n======================================================\n");
}