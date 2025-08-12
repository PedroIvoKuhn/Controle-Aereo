#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>

#define TEMPO_DE_SIMULACAO 1
#define NUM_PISTAS 3
#define NUM_PORTOES 5
#define TORRE_CAPACIDADE 2
#define SEGUNDOS_ALERTA_CRITICO 10
#define SEGUNDOS_QUEDA 15
#define MAX_VOOS 200

typedef enum { DOMESTICO, INTERNACIONAL } TipoVoo;
typedef enum { AGENDADO, POUSANDO, DESEMBARCANDO, AGUARDANDO_DECOLAGEM, DECOLANDO, CONCLUIDO, ACIDENTE, DEADLOCK } StatusVoo;
typedef enum { RECURSO_ADQUIRIDO = 1, SEM_RECURSO = 0, RESOURCE_TIMED_OUT = 2 } SolicitarStatus;

typedef struct {
    int id;
    TipoVoo tipo;
    StatusVoo status;
    pthread_t thread_id;
    time_t tempo_espera_inicio;
    int is_critical;
} InfoVoo;

typedef struct {
    int pistas_disponiveis;
    pthread_mutex_t mutex_pista;
    pthread_cond_t cond_pista_crit;
    pthread_cond_t cond_pista_intl;
    pthread_cond_t cond_pista_dom;
    int esperando_pista_crit;
    int esperando_pista_intl;

    int portoes_disponiveis;
    pthread_mutex_t mutex_portao;
    pthread_cond_t cond_portao_crit;
    pthread_cond_t cond_portao_intl;
    pthread_cond_t cond_portao_dom;
    int esperando_portao_crit;
    int esperando_portao_intl;

    int usuarios_torre;
    pthread_mutex_t mutex_torre;
    pthread_cond_t cond_torre_crit;
    pthread_cond_t cond_torre_intl;
    pthread_cond_t cond_torre_dom;
    int esperando_torre_crit;
    int esperando_torre_intl;
} RecursosAeroporto;

typedef struct {
    int voos_sucesso;
    int voos_acidentados;
    int incidentes_deadlock;
    int alertas_starvation;
    pthread_mutex_t mutex_stats;
} StatsSimulacao;

RecursosAeroporto aeroporto;
StatsSimulacao stats;
volatile int simulacao_rodando = 1;
InfoVoo todos_voos[MAX_VOOS];
int total_voos_criados = 0;
pthread_mutex_t mutex_log = PTHREAD_MUTEX_INITIALIZER;

void inicializar_aeroporto();
void destruir_aeroporto();
void* ciclo_vida_aviao(void *arg);
void imprimir_relatorio_final();
const char* get_tipo_str(TipoVoo tipo);
const char* get_status_str(StatusVoo status);
void log_evento(int id, TipoVoo tipo, const char* mensagem);
int solicitar_recurso(InfoVoo *voo, const char* nome_recurso, pthread_mutex_t *mutex, pthread_cond_t *cond_crit, pthread_cond_t *cond_intl, pthread_cond_t *cond_dom, int *recurso_disp, int *esperando_crit, int *esperando_intl);
void liberar_recurso(pthread_mutex_t *mutex, pthread_cond_t *cond_crit, pthread_cond_t *cond_intl, pthread_cond_t *cond_dom, int *recurso_disp, int *esperando_crit, int *esperando_intl);
int solicitar_torre(InfoVoo *voo);
void liberar_torre(InfoVoo *voo);

int main() {
    srand(time(NULL));
    inicializar_aeroporto();

    printf("--- Simulacao de Controle de Trafego Aereo Iniciada ---\n");
    printf("--- Ordem de Prioridade: 1.Critico -> 2.Internacional -> 3.Domestico ---\n");

    time_t inicio_simulacao = time(NULL);
    pthread_t threads[MAX_VOOS];

    while (time(NULL) - inicio_simulacao < TEMPO_DE_SIMULACAO * 60) {
        if (total_voos_criados < MAX_VOOS) {
            InfoVoo* info = &todos_voos[total_voos_criados];
            info->id = total_voos_criados + 1;
            info->tipo = (rand() % 3 == 0) ? INTERNACIONAL : DOMESTICO;
            info->status = AGENDADO; info->is_critical = 0;
            pthread_create(&threads[total_voos_criados], NULL, ciclo_vida_aviao, (void*)info);
            total_voos_criados++;
        }
        sleep(rand() % 3 + 1);
    }

    printf("\n--- TEMPO DE SIMULACAO ESGOTADO. Aguardando operacoes... ---\n");
    simulacao_rodando = 0;

    for (int i = 0; i < total_voos_criados; i++) {
        pthread_join(threads[i], NULL);
    }

    imprimir_relatorio_final();
    destruir_aeroporto();
    return 0;
}

void* ciclo_vida_aviao(void* arg) {
    InfoVoo *voo = (InfoVoo*)arg;
    
    // POUSO
    voo->status = POUSANDO;
    if (voo->tipo == INTERNACIONAL) {
        if (!solicitar_recurso(voo, "Pista", &aeroporto.mutex_pista, &aeroporto.cond_pista_crit, &aeroporto.cond_pista_intl, &aeroporto.cond_pista_dom, &aeroporto.pistas_disponiveis, &aeroporto.esperando_pista_crit, &aeroporto.esperando_pista_intl)) return NULL;
        if (!solicitar_torre(voo)) { liberar_recurso(&aeroporto.mutex_pista, &aeroporto.cond_pista_crit, &aeroporto.cond_pista_intl, &aeroporto.cond_pista_dom, &aeroporto.pistas_disponiveis, &aeroporto.esperando_pista_crit, &aeroporto.esperando_pista_intl); return NULL; }
    } else {
        if (!solicitar_torre(voo)) return NULL;
        if (!solicitar_recurso(voo, "Pista", &aeroporto.mutex_pista, &aeroporto.cond_pista_crit, &aeroporto.cond_pista_intl, &aeroporto.cond_pista_dom, &aeroporto.pistas_disponiveis, &aeroporto.esperando_pista_crit, &aeroporto.esperando_pista_intl)) { liberar_torre(voo); return NULL; }
    }
    log_evento(voo->id, voo->tipo, "pista e torre alocadas. Pousando...");
    sleep(3);
    liberar_recurso(&aeroporto.mutex_pista, &aeroporto.cond_pista_crit, &aeroporto.cond_pista_intl, &aeroporto.cond_pista_dom, &aeroporto.pistas_disponiveis, &aeroporto.esperando_pista_crit, &aeroporto.esperando_pista_intl);
    liberar_torre(voo);

    // DESEMBARQUE
    voo->status = DESEMBARCANDO;
    if (!solicitar_recurso(voo, "Portao", &aeroporto.mutex_portao, &aeroporto.cond_portao_crit, &aeroporto.cond_portao_intl, &aeroporto.cond_portao_dom, &aeroporto.portoes_disponiveis, &aeroporto.esperando_portao_crit, &aeroporto.esperando_portao_intl)) return NULL;
    if (!solicitar_torre(voo)) { liberar_recurso(&aeroporto.mutex_portao, &aeroporto.cond_portao_crit, &aeroporto.cond_portao_intl, &aeroporto.cond_portao_dom, &aeroporto.portoes_disponiveis, &aeroporto.esperando_portao_crit, &aeroporto.esperando_portao_intl); return NULL; }
    log_evento(voo->id, voo->tipo, "portao e torre alocados. Desembarcando...");
    liberar_torre(voo);
    sleep(5);
    
    // DECOLAGEM
    voo->status = AGUARDANDO_DECOLAGEM;
    sleep(rand() % 10 + 5);
    voo->status = DECOLANDO;
    if (voo->tipo == INTERNACIONAL) {
        if (!solicitar_recurso(voo, "Pista", &aeroporto.mutex_pista, &aeroporto.cond_pista_crit, &aeroporto.cond_pista_intl, &aeroporto.cond_pista_dom, &aeroporto.pistas_disponiveis, &aeroporto.esperando_pista_crit, &aeroporto.esperando_pista_intl)) { liberar_recurso(&aeroporto.mutex_portao, &aeroporto.cond_portao_crit, &aeroporto.cond_portao_intl, &aeroporto.cond_portao_dom, &aeroporto.portoes_disponiveis, &aeroporto.esperando_portao_crit, &aeroporto.esperando_portao_intl); return NULL; }
        if (!solicitar_torre(voo)) { 
            liberar_recurso(&aeroporto.mutex_portao, &aeroporto.cond_portao_crit, &aeroporto.cond_portao_intl, &aeroporto.cond_portao_dom, &aeroporto.portoes_disponiveis, &aeroporto.esperando_portao_crit, &aeroporto.esperando_portao_intl);
            liberar_recurso(&aeroporto.mutex_pista, &aeroporto.cond_pista_crit, &aeroporto.cond_pista_intl, &aeroporto.cond_pista_dom, &aeroporto.pistas_disponiveis, &aeroporto.esperando_pista_crit, &aeroporto.esperando_pista_intl);
            return NULL; 
        }
    } else {
        if (!solicitar_torre(voo)) { liberar_recurso(&aeroporto.mutex_portao, &aeroporto.cond_portao_crit, &aeroporto.cond_portao_intl, &aeroporto.cond_portao_dom, &aeroporto.portoes_disponiveis, &aeroporto.esperando_portao_crit, &aeroporto.esperando_portao_intl); return NULL; }
        if (!solicitar_recurso(voo, "Pista", &aeroporto.mutex_pista, &aeroporto.cond_pista_crit, &aeroporto.cond_pista_intl, &aeroporto.cond_pista_dom, &aeroporto.pistas_disponiveis, &aeroporto.esperando_pista_crit, &aeroporto.esperando_pista_intl)) { 
            liberar_recurso(&aeroporto.mutex_portao, &aeroporto.cond_portao_crit, &aeroporto.cond_portao_intl, &aeroporto.cond_portao_dom, &aeroporto.portoes_disponiveis, &aeroporto.esperando_portao_crit, &aeroporto.esperando_portao_intl);
            liberar_torre(voo); 
            return NULL; 
        }
    }
    log_evento(voo->id, voo->tipo, "portao, pista e torre alocados. Decolando...");
    sleep(3);
    liberar_recurso(&aeroporto.mutex_portao, &aeroporto.cond_portao_crit, &aeroporto.cond_portao_intl, &aeroporto.cond_portao_dom, &aeroporto.portoes_disponiveis, &aeroporto.esperando_portao_crit, &aeroporto.esperando_portao_intl);
    liberar_recurso(&aeroporto.mutex_pista, &aeroporto.cond_pista_crit, &aeroporto.cond_pista_intl, &aeroporto.cond_pista_dom, &aeroporto.pistas_disponiveis, &aeroporto.esperando_pista_crit, &aeroporto.esperando_pista_intl);
    liberar_torre(voo);
    
    voo->status = CONCLUIDO;
    pthread_mutex_lock(&stats.mutex_stats); stats.voos_sucesso++; pthread_mutex_unlock(&stats.mutex_stats);
    return NULL;
}

int solicitar_recurso(InfoVoo *voo, const char* recurso, pthread_mutex_t *mutex, pthread_cond_t *cond_crit, pthread_cond_t *cond_intl, pthread_cond_t *cond_dom, int *recurso_disp, int *esperando_crit, int *esperando_intl) {
    char msg[100];
    sprintf(msg, "solicitando %s.", recurso);
    log_evento(voo->id, voo->tipo, msg);
    pthread_mutex_lock(mutex);

    voo->tempo_espera_inicio = time(NULL);
    
    while (*recurso_disp == 0 ||
           (!voo->is_critical && *esperando_crit > 0) ||
           (voo->tipo == DOMESTICO && !voo->is_critical && *esperando_intl > 0))
    {

        pthread_cond_t* cond_a_esperar;
        if (voo->is_critical) {
            cond_a_esperar = cond_crit;
        } else if (voo->tipo == INTERNACIONAL) {
            cond_a_esperar = cond_intl;
            (*esperando_intl)++;
        } else {
            cond_a_esperar = cond_dom;
        }

        struct timespec timeout;
        clock_gettime(CLOCK_REALTIME, &timeout);
        timeout.tv_sec += 1;
        int rc = pthread_cond_timedwait(cond_a_esperar, mutex, &timeout);

        if (!voo->is_critical && voo->tipo == INTERNACIONAL) {
            (*esperando_intl)--;
        }

        if (rc == ETIMEDOUT && !voo->is_critical) {
            time_t tempo_espera = time(NULL) - voo->tempo_espera_inicio;
            if (tempo_espera >= SEGUNDOS_QUEDA) {
                sprintf(msg, "CAIU! Tempo de espera por %s excedeu.", recurso);
                log_evento(voo->id, voo->tipo, msg);
                voo->status = ACIDENTE;
                pthread_mutex_lock(&stats.mutex_stats); stats.voos_acidentados++; pthread_mutex_unlock(&stats.mutex_stats);
                pthread_mutex_unlock(mutex);
                return 0;
            } else if (tempo_espera >= SEGUNDOS_ALERTA_CRITICO) {
                sprintf(msg, "MAYDAY! MAYDAY! Risco de queda por %s!", recurso);
                log_evento(voo->id, voo->tipo, msg);
                voo->is_critical = 1;
                (*esperando_crit)++;
                pthread_mutex_lock(&stats.mutex_stats); stats.alertas_starvation++; pthread_mutex_unlock(&stats.mutex_stats);
            }
        }
    }
    
    (*recurso_disp)--;
    if (voo->is_critical) {
        voo->is_critical = 0;
        (*esperando_crit)--;
    }
    pthread_mutex_unlock(mutex);
    return 1;
}

void liberar_recurso(pthread_mutex_t *mutex, pthread_cond_t *cond_crit, pthread_cond_t *cond_intl, pthread_cond_t *cond_dom, int *recurso_disp, int *esperando_crit, int *esperando_intl) {
    pthread_mutex_lock(mutex);
    (*recurso_disp)++;

    if (*esperando_crit > 0) {
        pthread_cond_broadcast(cond_crit);
    } else if (*esperando_intl > 0) {
        pthread_cond_broadcast(cond_intl);
    } else {
        pthread_cond_broadcast(cond_dom);
    }
    pthread_mutex_unlock(mutex);
}

int solicitar_torre(InfoVoo *voo) {
    char msg[100];
    sprintf(msg, "solicitando acesso a torre.");
    log_evento(voo->id, voo->tipo, msg);
    pthread_mutex_lock(&aeroporto.mutex_torre);

    voo->tempo_espera_inicio = time(NULL);
    
    while (aeroporto.usuarios_torre >= TORRE_CAPACIDADE || 
          (!voo->is_critical && aeroporto.esperando_torre_crit > 0) ||
          (voo->tipo == DOMESTICO && !voo->is_critical && aeroporto.esperando_torre_intl > 0)) {
        pthread_cond_t* cond_a_esperar;
        if (voo->is_critical) {
            cond_a_esperar = &aeroporto.cond_torre_crit;
        } else if (voo->tipo == INTERNACIONAL) {
            cond_a_esperar = &aeroporto.cond_torre_intl;
            aeroporto.esperando_torre_intl++;
        } else {
            cond_a_esperar = &aeroporto.cond_torre_dom;
        }

        struct timespec timeout;
        clock_gettime(CLOCK_REALTIME, &timeout);
        timeout.tv_sec += 1;
        int rc = pthread_cond_timedwait(cond_a_esperar, &aeroporto.mutex_torre, &timeout);

        if (!voo->is_critical && voo->tipo == INTERNACIONAL) {
            aeroporto.esperando_torre_intl--;
        }

        if (rc == ETIMEDOUT && !voo->is_critical) {
            time_t tempo_espera = time(NULL) - voo->tempo_espera_inicio;
            if (tempo_espera >= SEGUNDOS_QUEDA) {
                log_evento(voo->id, voo->tipo, "CAIU! Tempo de espera pela torre excedeu.");
                voo->status = ACIDENTE;
                pthread_mutex_lock(&stats.mutex_stats); stats.voos_acidentados++; pthread_mutex_unlock(&stats.mutex_stats);
                pthread_mutex_unlock(&aeroporto.mutex_torre);
                return 0;
            } else if (tempo_espera >= SEGUNDOS_ALERTA_CRITICO) {
                log_evento(voo->id, voo->tipo, "MAYDAY! MAYDAY! Risco de queda pela torre!");
                voo->is_critical = 1;
                aeroporto.esperando_torre_crit++;
                pthread_mutex_lock(&stats.mutex_stats); stats.alertas_starvation++; pthread_mutex_unlock(&stats.mutex_stats);
            }
        }
    }
    
    aeroporto.usuarios_torre++;
    if (voo->is_critical) {
        voo->is_critical = 0;
        aeroporto.esperando_torre_crit--;
    }

    pthread_mutex_unlock(&aeroporto.mutex_torre);
    return 1;
}

void liberar_torre(InfoVoo *voo) {
    pthread_mutex_lock(&aeroporto.mutex_torre);
    aeroporto.usuarios_torre--;

    if (aeroporto.esperando_torre_crit > 0) {
        pthread_cond_broadcast(&aeroporto.cond_torre_crit);
    } else if (aeroporto.esperando_torre_intl > 0) {
        pthread_cond_broadcast(&aeroporto.cond_torre_intl);
    } else {
        pthread_cond_broadcast(&aeroporto.cond_torre_dom);
    }
    pthread_mutex_unlock(&aeroporto.mutex_torre);
}

void inicializar_aeroporto() {
    aeroporto.pistas_disponiveis = NUM_PISTAS;
    aeroporto.portoes_disponiveis = NUM_PORTOES;
    aeroporto.usuarios_torre = 0;
    aeroporto.esperando_pista_crit = 0;
    aeroporto.esperando_pista_intl = 0;
    aeroporto.esperando_portao_crit = 0;
    aeroporto.esperando_portao_intl = 0;
    aeroporto.esperando_torre_crit = 0;
    aeroporto.esperando_torre_intl = 0;
    
    pthread_mutex_init(&aeroporto.mutex_pista, NULL);
    pthread_mutex_init(&aeroporto.mutex_portao, NULL);
    pthread_mutex_init(&aeroporto.mutex_torre, NULL);
    pthread_mutex_init(&stats.mutex_stats, NULL);
    
    pthread_cond_init(&aeroporto.cond_pista_crit, NULL);
    pthread_cond_init(&aeroporto.cond_pista_intl, NULL);
    pthread_cond_init(&aeroporto.cond_pista_dom, NULL);
    pthread_cond_init(&aeroporto.cond_portao_crit, NULL);
    pthread_cond_init(&aeroporto.cond_portao_intl, NULL);
    pthread_cond_init(&aeroporto.cond_portao_dom, NULL);
    pthread_cond_init(&aeroporto.cond_torre_crit, NULL);
    pthread_cond_init(&aeroporto.cond_torre_intl, NULL);
    pthread_cond_init(&aeroporto.cond_torre_dom, NULL);

    stats.voos_sucesso = 0; stats.voos_acidentados = 0;
    stats.incidentes_deadlock = 0; stats.alertas_starvation = 0;
}

void destruir_aeroporto() {
    pthread_mutex_destroy(&aeroporto.mutex_pista);
    pthread_mutex_destroy(&aeroporto.mutex_portao);
    pthread_mutex_destroy(&aeroporto.mutex_torre);
    pthread_mutex_destroy(&stats.mutex_stats);
    pthread_mutex_destroy(&mutex_log);
    
    pthread_cond_destroy(&aeroporto.cond_pista_crit);
    pthread_cond_destroy(&aeroporto.cond_pista_intl);
    pthread_cond_destroy(&aeroporto.cond_pista_dom);
    pthread_cond_destroy(&aeroporto.cond_portao_crit);
    pthread_cond_destroy(&aeroporto.cond_portao_intl);
    pthread_cond_destroy(&aeroporto.cond_portao_dom);
    pthread_cond_destroy(&aeroporto.cond_torre_crit);
    pthread_cond_destroy(&aeroporto.cond_torre_intl);
    pthread_cond_destroy(&aeroporto.cond_torre_dom);
}

// LOGS ===========================================================================================
void log_evento(int id, TipoVoo tipo, const char* mensagem) {
    time_t agora = time(NULL); struct tm * ptm = localtime(&agora);
    char buf[30]; strftime(buf, 30, "%H:%M:%S", ptm);
    pthread_mutex_lock(&mutex_log);
    printf("[%s] Voo %03d (%-13s): %s\n", buf, id, get_tipo_str(tipo), mensagem);
    fflush(stdout);
    pthread_mutex_unlock(&mutex_log);
}

const char* get_tipo_str(TipoVoo tipo) { return tipo == INTERNACIONAL ? "Internacional" : "Domestico"; }
const char* get_status_str(StatusVoo status) {
    switch (status) {
        case AGENDADO: return "Agendado"; case POUSANDO: return "Pousando";
        case DESEMBARCANDO: return "Desembarcando"; case AGUARDANDO_DECOLAGEM: return "Aguardando Decolagem";
        case DECOLANDO: return "Decolando"; case CONCLUIDO: return "Concluido";
        case ACIDENTE: return "ACIDENTE (Starvation)"; case DEADLOCK: return "DEADLOCK";
        default: return "Desconhecido";
    }
}

void imprimir_relatorio_final() {
    printf("\n\n======================================================\n");
    printf("              RELATORIO FINAL DA SIMULACAO\n");
    printf("======================================================\n\n");
    printf("--- METRICAS GERAIS ---\n");
    printf("Total de voos criados: %d\n", total_voos_criados);
    printf("Voos concluidos com sucesso: %d\n", stats.voos_sucesso);
    printf("Voos acidentados (starvation): %d\n", stats.voos_acidentados);
    printf("Alertas de starvation (MAYDAY) emitidos: %d\n", stats.alertas_starvation);
    printf("Incidentes de deadlock detectados: %d\n", stats.incidentes_deadlock);
    printf("\n--- ESTADO FINAL DOS VOOS ---\n");
    for (int i = 0; i < total_voos_criados; i++) {
        InfoVoo *voo = &todos_voos[i];
        printf("Voo %03d (%-13s) - Status Final: %s\n", voo->id, get_tipo_str(voo->tipo), get_status_str(voo->status));
    }
    printf("\n======================================================\n");
}