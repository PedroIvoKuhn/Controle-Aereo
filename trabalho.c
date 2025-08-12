#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>

#define TEMPO_SIMULACAO_MINUTOS 5 // 5m // 1m
#define NUMERO_PISTAS 3             
#define NUMERO_PORTOES 5            
#define CAPACIDADE_TORRE 2          
#define MAX_VOOS 200
#define TIMEOUT_TENTATIVA_SEGUNDOS 1 
#define ALERTA_FOME_SEGUNDOS 60   // 60s // 12s
#define QUEDA_AVIAO_SEGUNDOS 90   // 90s  // 18s
#define TEMPO_BASE_OPERACAO 2 // Tempo para cada operação
// MULTIPLICADORES DOS RECURSOS
// PISTA = 0.5
// TORRE = 0.1
// PORTAO = 1

typedef enum { DOMESTICO, INTERNACIONAL } TipoVoo;
typedef enum { AGUARDANDO, POUSANDO, DESEMBARCANDO, DECOLANDO, CONCLUIDO, ACIDENTE } StatusVoo;

typedef struct {
    int id;
    TipoVoo tipo;
    StatusVoo status;
    int eCritico;
    time_t inicioDaEspera;
} InfoVoo;

typedef struct {
    int pistasDisponiveis;
    pthread_mutex_t mutexPista;
    pthread_cond_t condPistaCritico, condPistaInternacional, condPistaDomestico;
    int esperandoPistaCritico, esperandoPistaInternacional;

    int portoesDisponiveis;
    pthread_mutex_t mutexPortao;
    pthread_cond_t condPortaoCritico, condPortaoInternacional, condPortaoDomestico;
    int esperandoPortaoCritico, esperandoPortaoInternacional;

    int torreDisponivel;
    pthread_mutex_t mutexTorre;
    pthread_cond_t condTorreCritico, condTorreInternacional, condTorreDomestico;
    int esperandoTorreCritico, esperandoTorreInternacional;
} RecursosAeroporto;

typedef struct {
    int voosSucesso, voosAcidentados, deadlocksEvitados, alertasDeStarvation;
    pthread_mutex_t mutexEstatisticas;
} Estatisticas;

RecursosAeroporto aeroporto;
Estatisticas estatisticas;
InfoVoo todosOsVoos[MAX_VOOS];
int totalDeVoosCriados = 0;

void inicializarAeroporto();
void destruirAeroporto();
void* cicloDeVidaDoAviao(void *arg);
void imprimirRelatorioFinal();
void logEvento(int id, TipoVoo tipo, const char* mensagem);
int solicitarRecurso(InfoVoo *voo, const char* nomeRecurso, pthread_mutex_t *mutex, pthread_cond_t *conds[3], int *recursoDisponivel, int *esperandoCritico, int *esperandoInternacional);
void liberarRecurso(pthread_mutex_t *mutex, pthread_cond_t *conds[3], int *recursoDisponivel, int *esperandoCritico, int *esperandoInternacional);
const char* getStatusEmTexto(StatusVoo status);

int main() {
    srand(time(NULL));
    inicializarAeroporto();
    printf("--- Simulacao De controle de Trafego Aereo ---\n");
    printf("--- Ordem de Prioridade: 1.Critico -> 2.Internacional -> 3.Domestico ---\n");
    time_t inicio_simulacao = time(NULL);
    pthread_t threads[MAX_VOOS];
    while (time(NULL) - inicio_simulacao < TEMPO_SIMULACAO_MINUTOS * 60) {
        if (totalDeVoosCriados < MAX_VOOS) {
            InfoVoo* info = &todosOsVoos[totalDeVoosCriados];
            info->id = totalDeVoosCriados + 1;
            info->tipo = (rand() % 3 == 0) ? INTERNACIONAL : DOMESTICO;
            info->status = AGUARDANDO;
            info->eCritico = 0;
            pthread_create(&threads[totalDeVoosCriados], NULL, cicloDeVidaDoAviao, (void*)info);
            totalDeVoosCriados++;
        }
        sleep(TEMPO_BASE_OPERACAO);
    }
    printf("\n--- TEMPO DE SIMULACAO ESGOTADO. Aguardando operacoes restantes... ---\n");
    for (int i = 0; i < totalDeVoosCriados; i++) {
        pthread_join(threads[i], NULL);
    }
    imprimirRelatorioFinal();
    destruirAeroporto();
    return 0;
}

void* cicloDeVidaDoAviao(void* arg) {
    // Define o arg para a struct de voo
    InfoVoo *voo = (InfoVoo*)arg;
    // Arrays para as condicionais de cada recurso e prioridade
    // São passados para as funções que pedem/liberam recursos
    pthread_cond_t *condsPista[3] = {&aeroporto.condPistaCritico, &aeroporto.condPistaInternacional, &aeroporto.condPistaDomestico};
    pthread_cond_t *condsTorre[3] = {&aeroporto.condTorreCritico, &aeroporto.condTorreInternacional, &aeroporto.condTorreDomestico};
    pthread_cond_t *condsPortao[3] = {&aeroporto.condPortaoCritico, &aeroporto.condPortaoInternacional, &aeroporto.condPortaoDomestico};

    // ---========= POUSO =========---
    voo->status = POUSANDO; // Define o status do voo para POUSANDO
    voo->inicioDaEspera = time(NULL); // Marca o inicio da espera
    logEvento(voo->id, voo->tipo, "iniciando procedimento de pouso.");
    while (1) { // Loop até conseguir os dois recursos necessarios
        if (voo->tipo == INTERNACIONAL) { // Voo Internacional. Ordem: Pista -> Torre
            // Se solicitar_recurso retornar 1, conseguiu pista, tenta torre...
            if (solicitarRecurso(voo, "Pista", &aeroporto.mutexPista, condsPista, &aeroporto.pistasDisponiveis, &aeroporto.esperandoPistaCritico, &aeroporto.esperandoPistaInternacional)) {
                // Se conseguir torre(retorna 1) faz o break e sai do Loop
                if (solicitarRecurso(voo, "Torre", &aeroporto.mutexTorre, condsTorre, &aeroporto.torreDisponivel, &aeroporto.esperandoTorreCritico, &aeroporto.esperandoTorreInternacional)) {
                    break; // Sai do Loop
                } else {
                    // Se não conseguir torre, verifica se o tempo acabou(caiu)
                    if (voo->status == ACIDENTE) return NULL;
                    // Se não caiu loga e libera a pista que tinha pegado.
                    logEvento(voo->id, voo->tipo, "Timeout para pegar Torre. Devolvendo Pista e tentando de novo...");
                    liberarRecurso(&aeroporto.mutexPista, condsPista, &aeroporto.pistasDisponiveis, &aeroporto.esperandoPistaCritico, &aeroporto.esperandoPistaInternacional);
                }
            }
        } else { // Voo Doméstico Ordem: Torre -> Pista
            // Se solicitar_recurso retornar 1, conseguiu torre, tenta pista...
            if (solicitarRecurso(voo, "Torre", &aeroporto.mutexTorre, condsTorre, &aeroporto.torreDisponivel, &aeroporto.esperandoTorreCritico, &aeroporto.esperandoTorreInternacional)) {
                // // Se conseguir pista(retorna 1) faz o break e sai do Loop
                if (solicitarRecurso(voo, "Pista", &aeroporto.mutexPista, condsPista, &aeroporto.pistasDisponiveis, &aeroporto.esperandoPistaCritico, &aeroporto.esperandoPistaInternacional)) {
                    break; // Sai do Loop
                } else {
                    // Se não conseguir uma pista, verifica se o tempo acabou(caiu)
                    if (voo->status == ACIDENTE) return NULL;
                    // Se não caiu loga e libera a torre que tinha pegado.
                    logEvento(voo->id, voo->tipo, "Timeout para pegar Pista. Devolvendo Torre e tentando de novo...");
                    liberarRecurso(&aeroporto.mutexTorre, condsTorre, &aeroporto.torreDisponivel, &aeroporto.esperandoTorreCritico, &aeroporto.esperandoTorreInternacional);
                }
            }
        }
        if(voo->status == ACIDENTE) return NULL; // Após as tentativas verifica se caiu
        sleep(rand() % 3 + 1); // Espera aleatoriamente entre as tentativas 
    }
    // Conseguiu sair do Loop, então conseguiu pousar e libera o recurso
    sleep((TEMPO_BASE_OPERACAO * 0.1) + (TEMPO_BASE_OPERACAO * 0.5)); // Precisa de torre e pista
    logEvento(voo->id, voo->tipo, "pouso concluido. Liberando recursos.");
    liberarRecurso(&aeroporto.mutexPista, condsPista, &aeroporto.pistasDisponiveis, &aeroporto.esperandoPistaCritico, &aeroporto.esperandoPistaInternacional);
    liberarRecurso(&aeroporto.mutexTorre, condsTorre, &aeroporto.torreDisponivel, &aeroporto.esperandoTorreCritico, &aeroporto.esperandoTorreInternacional);

    // ---========= DESEMBARQUE =========---
    voo->status = DESEMBARCANDO; // Define o status do voo para DESEMBARCANDO
    // voo->inicioDaEspera = time(NULL);
    while (1) { // Loop até conseguir os dois recursos necessarios
        if (voo->tipo == INTERNACIONAL) { // Voo Internacional. Ordem: Portão -> Torre
            // Se solicitar_recurso retornar 1, conseguiu portão, tenta torre...
            if (solicitarRecurso(voo, "Portao", &aeroporto.mutexPortao, condsPortao, &aeroporto.portoesDisponiveis, &aeroporto.esperandoPortaoCritico, &aeroporto.esperandoPortaoInternacional)) {
                // Se conseguir torre(retorna 1) faz o break e sai do Loop
                if (solicitarRecurso(voo, "Torre", &aeroporto.mutexTorre, condsTorre, &aeroporto.torreDisponivel, &aeroporto.esperandoTorreCritico, &aeroporto.esperandoTorreInternacional)) {
                    break; // Sai do Loop
                } else {
                    // Se não conseguir torre, verifica se o tempo acabou(caiu)
                    if (voo->status == ACIDENTE) return NULL;
                    // Se não caiu loga e libera o portão que tinha pegado.
                    logEvento(voo->id, voo->tipo, "Timeout para pegar Torre. Devolvendo Portao e tentando de novo...");
                    liberarRecurso(&aeroporto.mutexPortao, condsPortao, &aeroporto.portoesDisponiveis, &aeroporto.esperandoPortaoCritico, &aeroporto.esperandoPortaoInternacional);
                }
            }
        } else { // Voo Doméstico Ordem: Torre -> Portão
            // Se solicitar_recurso retornar 1, conseguiu torre, tenta pista...
            if (solicitarRecurso(voo, "Torre", &aeroporto.mutexTorre, condsTorre, &aeroporto.torreDisponivel, &aeroporto.esperandoTorreCritico, &aeroporto.esperandoTorreInternacional)) {
                // // Se conseguir o portão(retorna 1) faz o break e sai do Loop
                if (solicitarRecurso(voo, "Portao", &aeroporto.mutexPortao, condsPortao, &aeroporto.portoesDisponiveis, &aeroporto.esperandoPortaoCritico, &aeroporto.esperandoPortaoInternacional)) {
                    break; // Sai do Loop
                } else {
                    // Se não conseguir um portão, verifica se o tempo acabou(caiu)
                    if (voo->status == ACIDENTE) return NULL;
                    // Se não caiu loga e libera a torre que tinha pegado.
                    logEvento(voo->id, voo->tipo, "Timeout para pegar Portao. Devolvendo Torre e tentando de novo...");
                    liberarRecurso(&aeroporto.mutexTorre, condsTorre, &aeroporto.torreDisponivel, &aeroporto.esperandoTorreCritico, &aeroporto.esperandoTorreInternacional);
                }
            }
        }
        if(voo->status == ACIDENTE) return NULL; // Após as tentativas verifica se caiu
        sleep(rand() % 3 + 1); // Espera aleatoriamente entre as tentativas 
    }
    // Conseguiu sair do Loop, loga desembarcando e libera recursos
    logEvento(voo->id, voo->tipo, "desembarcando...");
    sleep(((TEMPO_BASE_OPERACAO * 1) + (TEMPO_BASE_OPERACAO * 0.1))); // precisa de portao e torre
    liberarRecurso(&aeroporto.mutexTorre, condsTorre, &aeroporto.torreDisponivel, &aeroporto.esperandoTorreCritico, &aeroporto.esperandoTorreInternacional);
    liberarRecurso(&aeroporto.mutexPortao, condsPortao, &aeroporto.portoesDisponiveis, &aeroporto.esperandoPortaoCritico, &aeroporto.esperandoPortaoInternacional);

    // ---========= DECOLAGEM =========---
    voo->status = DECOLANDO; // Define o status do voo para DECOLANDO
    logEvento(voo->id, voo->tipo, "iniciando procedimento de decolagem.");
    // voo->inicioDaEspera = time(NULL);
    while (1) {
        if (voo->tipo == INTERNACIONAL) { // Voo Internacional. Ordem: Portão -> Pista -> Torre
            // Tenta pegar portão
            if (solicitarRecurso(voo, "Portao", &aeroporto.mutexPortao, condsPortao, &aeroporto.portoesDisponiveis, &aeroporto.esperandoPortaoCritico, &aeroporto.esperandoPortaoInternacional)) {
                // Tenta pegar pista
                if (solicitarRecurso(voo, "Pista", &aeroporto.mutexPista, condsPista, &aeroporto.pistasDisponiveis, &aeroporto.esperandoPistaCritico, &aeroporto.esperandoPistaInternacional)) {
                    // Tenta pegar torre
                    if (solicitarRecurso(voo, "Torre", &aeroporto.mutexTorre, condsTorre, &aeroporto.torreDisponivel, &aeroporto.esperandoTorreCritico, &aeroporto.esperandoTorreInternacional)) {
                        break; // Se conseguiu pegar tudo sai do loop
                    } else {
                        // Se não conseguiu pegar torre, libera os outros recursos
                        liberarRecurso(&aeroporto.mutexPortao, condsPortao, &aeroporto.portoesDisponiveis, &aeroporto.esperandoPortaoCritico, &aeroporto.esperandoPortaoInternacional);
                        liberarRecurso(&aeroporto.mutexPista, condsPista, &aeroporto.pistasDisponiveis, &aeroporto.esperandoPistaCritico, &aeroporto.esperandoPistaInternacional);
                        if (voo->status == ACIDENTE) return NULL; // verifica se o avião caiu e termina a thread
                    }
                } else {
                    // Se não conseguiu pista, libera o portão que tinha pegado
                    liberarRecurso(&aeroporto.mutexPortao, condsPortao, &aeroporto.portoesDisponiveis, &aeroporto.esperandoPortaoCritico, &aeroporto.esperandoPortaoInternacional);
                    if (voo->status == ACIDENTE) return NULL; // Se caiu retorna NULL
                }
            }
        } else { // Voo Doméstico Ordem: Torre -> Portão -> Pista
            // Tenta pegar TORRE
            if (solicitarRecurso(voo, "Torre", &aeroporto.mutexTorre, condsTorre, &aeroporto.torreDisponivel, &aeroporto.esperandoTorreCritico, &aeroporto.esperandoTorreInternacional)) {
                // Tenta pegar PORTÃO
                if (solicitarRecurso(voo, "Portao", &aeroporto.mutexPortao, condsPortao, &aeroporto.portoesDisponiveis, &aeroporto.esperandoPortaoCritico, &aeroporto.esperandoPortaoInternacional)) {
                    // Tenta pegar PISTA
                    if (solicitarRecurso(voo, "Pista", &aeroporto.mutexPista, condsPista, &aeroporto.pistasDisponiveis, &aeroporto.esperandoPistaCritico, &aeroporto.esperandoPistaInternacional)) {
                        break; // Se conseguiu pegar TUDO sai do loop
                    } else {
                        // Se não conseguiu pegar PISTA, libera os outros recursos
                        liberarRecurso(&aeroporto.mutexTorre, condsTorre, &aeroporto.torreDisponivel, &aeroporto.esperandoTorreCritico, &aeroporto.esperandoTorreInternacional);
                        liberarRecurso(&aeroporto.mutexPortao, condsPortao, &aeroporto.portoesDisponiveis, &aeroporto.esperandoPortaoCritico, &aeroporto.esperandoPortaoInternacional);
                        if (voo->status == ACIDENTE) return NULL; // verifica se o avião caiu e termina a thread
                    }
                } else {
                    // Se não conseguiu pista, libera o portão que tinha pegado
                    liberarRecurso(&aeroporto.mutexTorre, condsTorre, &aeroporto.torreDisponivel, &aeroporto.esperandoTorreCritico, &aeroporto.esperandoTorreInternacional);
                    if (voo->status == ACIDENTE) return NULL; // Se caiu retorna NULL
                }
            }
        }
        if(voo->status == ACIDENTE) return NULL; // Após as tentativas verifica se caiu
        sleep(rand() % 3 + 1); // Espera aleatoriamente entre as tentativas 
    }

    sleep((TEMPO_BASE_OPERACAO * 1) + (TEMPO_BASE_OPERACAO * 0.1) + (TEMPO_BASE_OPERACAO * 0.5)); // Precisa dos tres
    logEvento(voo->id, voo->tipo, "decolagem concluida. Liberando todos os recursos.");
    liberarRecurso(&aeroporto.mutexPortao, condsPortao, &aeroporto.portoesDisponiveis, &aeroporto.esperandoPortaoCritico, &aeroporto.esperandoPortaoInternacional);
    liberarRecurso(&aeroporto.mutexPista, condsPista, &aeroporto.pistasDisponiveis, &aeroporto.esperandoPistaCritico, &aeroporto.esperandoPistaInternacional);
    liberarRecurso(&aeroporto.mutexTorre, condsTorre, &aeroporto.torreDisponivel, &aeroporto.esperandoTorreCritico, &aeroporto.esperandoTorreInternacional);

    voo->status = CONCLUIDO; // Define o status do voo para CONCLUIDO
    pthread_mutex_lock(&estatisticas.mutexEstatisticas); 
    estatisticas.voosSucesso++; 
    pthread_mutex_unlock(&estatisticas.mutexEstatisticas);
    return NULL;
}

int solicitarRecurso(InfoVoo *voo, const char* nomeRecurso, pthread_mutex_t *mutex, pthread_cond_t *conds[3], int *recursoDisponivel, int *esperandoCritico, int *esperandoInternacional) {
    pthread_mutex_lock(mutex); // Garante acesso exclusivo à variável que representa o recurso.
    time_t inicio_desta_tentativa = time(NULL); // Marca o inicio dessa tentativa

    // Fica esperando se:
    // 1. Não tem recursos disponives
    // 2. Não é critico e não tem críticos esperando
    // 3. Doméstico, não crítico e internacionais na fila
    while (*recursoDisponivel == 0 ||
           (!voo->eCritico && *esperandoCritico > 0) ||
           (voo->tipo == DOMESTICO && !voo->eCritico && *esperandoInternacional > 0))
    {
        // Define a fila de espera:
        // conds[0] -> Críticos; [1] -> Internacionais; [2] -> Domesticos
        pthread_cond_t *fila_de_espera = voo->eCritico ? conds[0] : (voo->tipo == INTERNACIONAL ? conds[1] : conds[2]);
        // Marca se o avião está na fila internacional
        if (voo->tipo == INTERNACIONAL && !voo->eCritico) (*esperandoInternacional)++;

        // Espera um segundo antes de verificar sua situação
        struct timespec timeout;
        clock_gettime(CLOCK_REALTIME, &timeout);
        timeout.tv_sec += 1;
        // Função usada para não ficar travado indefinidamente
        int rc = pthread_cond_timedwait(fila_de_espera, mutex, &timeout);
        // tira da fila internacional
        if (voo->tipo == INTERNACIONAL && !voo->eCritico) (*esperandoInternacional)--;

        if (rc == ETIMEDOUT) {
            // Calcula tempo total de espera
            time_t tempo_total_espera = time(NULL) - voo->inicioDaEspera;
            
            // Se passou do tempo de queda, então caiu
            if (tempo_total_espera >= QUEDA_AVIAO_SEGUNDOS) {
                voo->status = ACIDENTE;
                // Trava o mutex para somar o acidente e libera depois
                pthread_mutex_lock(&estatisticas.mutexEstatisticas); 
                estatisticas.voosAcidentados++; 
                pthread_mutex_unlock(&estatisticas.mutexEstatisticas);
                logEvento(voo->id, voo->tipo, "CAIU! Tempo de espera excedeu o limite.");
                // Tira o avião da fila de criticos
                if (voo->eCritico) (*esperandoCritico)--;
                pthread_mutex_unlock(mutex);
                return 0;
            }
            // Se esperou demais, vai para crítico
            if (tempo_total_espera >= ALERTA_FOME_SEGUNDOS && !voo->eCritico) {
                logEvento(voo->id, voo->tipo, "MAYDAY! MAYDAY! Risco de fome!");
                voo->eCritico = 1;
                (*esperandoCritico)++;
                pthread_mutex_lock(&estatisticas.mutexEstatisticas);
                estatisticas.alertasDeStarvation++; 
                pthread_mutex_unlock(&estatisticas.mutexEstatisticas);
            }
            // Se passou do tempo limite da tentativa retorna 0
            if (time(NULL) - inicio_desta_tentativa >= TIMEOUT_TENTATIVA_SEGUNDOS) {
                pthread_mutex_unlock(mutex);
                return 0;
            }
        }
    }
    // Se conseguiu o recurso
    // caso seja crítico reseta status e marca ocupado o recurso
    (*recursoDisponivel)--;
    if (voo->eCritico) { voo->eCritico = 0; (*esperandoCritico)--; }
    pthread_mutex_unlock(mutex); // libera o mutex e retorna 1
    return 1;
}

void liberarRecurso(pthread_mutex_t *mutex, pthread_cond_t *conds[3], int *recursoDisponivel, int *esperandoCritico, int *esperandoInternacional) {
    pthread_mutex_lock(mutex); // Travao mutex
    (*recursoDisponivel)++; // Aumenta o contador de recursos disponives
    // Escolhe quem avisar primeiro:
    // 1. Criticos; 2. Internacionais; 3. Domesticos
    if (*esperandoCritico > 0) pthread_cond_broadcast(conds[0]);
    else if (*esperandoInternacional > 0) pthread_cond_broadcast(conds[1]);
    else pthread_cond_broadcast(conds[2]);
    // Libera o mutex
    pthread_mutex_unlock(mutex);
}

void inicializarAeroporto() {
    // Define numero de recursos
    aeroporto.pistasDisponiveis = NUMERO_PISTAS;
    aeroporto.portoesDisponiveis = NUMERO_PORTOES;
    aeroporto.torreDisponivel = CAPACIDADE_TORRE;
    // Inicializa as filas com 0
    aeroporto.esperandoPistaCritico = 0; 
    aeroporto.esperandoPistaInternacional = 0;
    aeroporto.esperandoPortaoCritico = 0; 
    aeroporto.esperandoPortaoInternacional = 0;
    aeroporto.esperandoTorreCritico = 0; 
    aeroporto.esperandoTorreInternacional = 0;
    // Cria mutexes para todos os recursos
    pthread_mutex_init(&aeroporto.mutexPista, NULL);
    pthread_mutex_init(&aeroporto.mutexPortao, NULL);
    pthread_mutex_init(&aeroporto.mutexTorre, NULL);
    pthread_mutex_init(&estatisticas.mutexEstatisticas, NULL);
    // Variaveis de condição para prioridades e recursos
    pthread_cond_init(&aeroporto.condPistaCritico, NULL); 
    pthread_cond_init(&aeroporto.condPistaInternacional, NULL); 
    pthread_cond_init(&aeroporto.condPistaDomestico, NULL);
    pthread_cond_init(&aeroporto.condPortaoCritico, NULL); 
    pthread_cond_init(&aeroporto.condPortaoInternacional, NULL); 
    pthread_cond_init(&aeroporto.condPortaoDomestico, NULL);
    pthread_cond_init(&aeroporto.condTorreCritico, NULL); 
    pthread_cond_init(&aeroporto.condTorreInternacional, NULL); 
    pthread_cond_init(&aeroporto.condTorreDomestico, NULL);
    // Define todas as estatisticas como 0
    estatisticas.voosSucesso = 0; 
    estatisticas.voosAcidentados = 0;
    estatisticas.deadlocksEvitados = 0; 
    estatisticas.alertasDeStarvation = 0;
}

void destruirAeroporto() {
    // Libera todos os mutexes criados
    pthread_mutex_destroy(&aeroporto.mutexPista); 
    pthread_mutex_destroy(&aeroporto.mutexPortao); 
    pthread_mutex_destroy(&aeroporto.mutexTorre);
    pthread_mutex_destroy(&estatisticas.mutexEstatisticas);
    // Libera as variaves de condição
    pthread_cond_destroy(&aeroporto.condPistaCritico); 
    pthread_cond_destroy(&aeroporto.condPistaInternacional);
    pthread_cond_destroy(&aeroporto.condPistaDomestico);
    pthread_cond_destroy(&aeroporto.condPortaoCritico); 
    pthread_cond_destroy(&aeroporto.condPortaoInternacional); 
    pthread_cond_destroy(&aeroporto.condPortaoDomestico);
    pthread_cond_destroy(&aeroporto.condTorreCritico); 
    pthread_cond_destroy(&aeroporto.condTorreInternacional); 
    pthread_cond_destroy(&aeroporto.condTorreDomestico);
}

void logEvento(int id, TipoVoo tipo, const char* mensagem) {
    time_t agora = time(NULL); struct tm * ptm = localtime(&agora);
    char buf[30]; strftime(buf, 30, "%H:%M:%S", ptm);
    printf("[%s] Voo %03d (%s): %s\n", buf, id, tipo == INTERNACIONAL ? "Internacional" : "Domestico   ", mensagem);
    fflush(stdout);
}

const char* getStatusEmTexto(StatusVoo status) {
    switch (status) {
        case AGUARDANDO: return "Aguardando";
        case POUSANDO: return "Em Pouso/Decolagem";
        case DESEMBARCANDO: return "Em Desembarque";
        case DECOLANDO: return "Em Decolagem";
        case CONCLUIDO: return "Concluido";
        case ACIDENTE: return "ACIDENTE";
        default: return "Desconhecido";
    }
}

void imprimirRelatorioFinal() {
    printf("\n\n======================================================\n");
    printf("              RELATORIO FINAL DA SIMULACAO\n");
    printf("======================================================\n");
    
    printf("\n--- METRICAS GERAIS ---\n");
    printf("Voos concluidos com sucesso: %d\n", estatisticas.voosSucesso);
    printf("Voos acidentados por starvation: %d\n", estatisticas.voosAcidentados);
    printf("Alertas (MAYDAY) emitidos: %d\n", estatisticas.alertasDeStarvation);
    printf("Potenciais Deadlocks Evitados (Backoffs): %d\n", estatisticas.deadlocksEvitados);
    printf("Pistas disponiveis: %d\n", aeroporto.pistasDisponiveis);
    printf("Portoes disponiveis: %d\n", aeroporto.portoesDisponiveis);
    printf("Capacidade da Torre: %d\n", aeroporto.torreDisponivel);

    printf("\n--- ESTADO FINAL DE CADA VOO ---\n");
    for (int i = 0; i < totalDeVoosCriados; i++) {
        InfoVoo *voo = &todosOsVoos[i];
        const char *tipo_str = voo->tipo == INTERNACIONAL ? "Internacional" : "Domestico   ";
        printf("Voo %03d (%s) - Status Final: %s\n", voo->id, tipo_str, getStatusEmTexto(voo->status));
    }
    printf("\n======================================================\n");
}