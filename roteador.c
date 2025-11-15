#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <semaphore.h>
#include <time.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>  // para mkfifo()
#include <signal.h> //pro sigatomic_t

#define CONFIG_FILE "roteador.config"
#define ENLACE_FILE "enlace.config"
#define Controle 0
#define Dado 1

//se estiver debugando == 1 se n qualquer outra coisa
#define debugando 1

#define numRoteadores 4


#define tamanhoMaximoFila 15


typedef struct{
    int tipo;//tipo de msg
    int origem;//quem enviou
    int destino;//qual é o destino
    // para fazer rota calcular no inicio
    char conteudo[500]; //texto que a msg vai mandar
} Mensagem;


typedef struct{

    int tamanho;
    Mensagem conteudo[tamanhoMaximoFila];

  
    pthread_mutex_t lock; //protege o acesso a fila
    sem_t cheio;  //fica olhando pra n ficar executando

} Fila;


// vai ser guardado na orde, se quer ir para o roteador 3 acessa  VetorDistancia[2]
typedef struct 
{
    // pra qual roteador vai sair
    int saida;
    //quanto custa até a saida
    int custo;

    //se é vizinho
    int isVisinho;
    //quanto tempo faz q n manda o vetor distancia
    int rodadasSemResposta;

} Distancia;

typedef struct 
{
    //todos os vetores
    Distancia vetores[numRoteadores];
    
    pthread_mutex_t lock; //mutex

    
} VetoresDistancia;



//este roteador só tem q saber isso
typedef struct 
{
    //destino quer dizer qual roteador tem q ir e a saida é por onde vai passar a msg
    int destino;
   
    //quanto custa até a saida
    int custo;

} DistanciaRecebido;



// cada roteador vai mandar um destes para calcular o vetor distancia
typedef struct 
{
    //todos as distancias de um roteador, como vai ser só analizado n tem q ter lock, o que junta todos vai ter
    DistanciaRecebido vetores[numRoteadores];
    
} vetoresRecebidos;


//quando chegar um novo vetor distancia armazena aqui para processar
typedef struct 
{
    //vetores distancias recebidos mas n analizados, vetor de vetores
    vetoresRecebidos vetoresNaoAnalizados[numRoteadores];
    //vetor que controla se ja foi calculado, zero se deve ser analizado, 1 se ja foi
    int testados[numRoteadores];

    pthread_mutex_t lock; //mutex

    
} AnalizarVetores;



//filas globais

Fila filaEntrada;
Fila filaSaida;
int id;  //mudar vai receber do arquivo, ai abre outroa rquivo para pegar seu ip
int meuSocket;
int roteadorInciado = 0;  
int globalFifoFd = -1; 
int vetorThreadRunning = 0;
volatile sig_atomic_t threadTerminated = 0; 
//vetor que guarda a topografia
VetoresDistancia vetorDistancia;
//vetor com as topografias para analize
AnalizarVetores vetoresParaAnalise;


void initFilas() {

    // filaEntrada
    filaEntrada.tamanho = 0;
    memset(filaEntrada.conteudo, 0, sizeof(filaEntrada.conteudo));
    pthread_mutex_init(&filaEntrada.lock, NULL);
    sem_init(&filaEntrada.cheio, 0, 0);


    // filaSaida
    filaSaida.tamanho = 0;
    memset(filaSaida.conteudo, 0, sizeof(filaSaida.conteudo));
    pthread_mutex_init(&filaSaida.lock, NULL);
    sem_init(&filaSaida.cheio, 0, 0);

}


//passar como ponteiro
//passar o semaforo para bloquear na hora certa !!!!!!!!!!!!!!! tem q mudar pra multithead
//ou sempre pegar o lock, se para daria pra controlar melhor
void addMsg(Mensagem msg ){

    pthread_mutex_lock(&filaEntrada.lock);
    

    //testa se a fila ta cheioa
    if(filaEntrada.tamanho >= tamanhoMaximoFila){
        printf("fila cheia");

        pthread_mutex_unlock(&filaEntrada.lock);
        return;
    }

   //muda  as var da fila
    filaEntrada.conteudo[filaEntrada.tamanho] = msg;
    filaEntrada.tamanho ++;

    //libera po lock e add o semafaro
    sem_post(&filaEntrada.cheio);
    pthread_mutex_unlock(&filaEntrada.lock);
    
    return;
}

//passar como ponteiro
//passar o semaforo para bloquear na hora certa !!!!!!!!!!!!!!! tem q mudar pra multithead
//ou sempre pegar o lock, se para daria pra controlar melhor
void sendMsg(Mensagem msg ){

    pthread_mutex_lock(&filaSaida.lock);
    

    //testa se a fila ta cheioa
    if(filaSaida.tamanho >= tamanhoMaximoFila){
        printf("fila cheia");

        pthread_mutex_unlock(&filaSaida.lock);
        return;
    }

   //muda  as var da fila
    filaSaida.conteudo[filaSaida.tamanho] = msg;
    filaSaida.tamanho ++;

    //libera po lock e add o semafaro
    sem_post(&filaSaida.cheio);
    pthread_mutex_unlock(&filaSaida.lock);
    
    return;
}




//passa o global, pega o topo da fila, quando adiciona só taca no topo
//Mensagem m = getMensagem(&minhafila);
Mensagem getMsg(){
    
    //msg de erro
    Mensagem vazio = {0};

    //tenta pegar o lock
    pthread_mutex_lock(&filaEntrada.lock);


    //get lock
    if(filaEntrada.tamanho == 0){

        pthread_mutex_unlock(&filaEntrada.lock);
        return vazio;

    }

    //passou os testes, pega a mensagem

    Mensagem retorno = filaEntrada.conteudo[0];
    
    //tira da lista zera os bytes
    memset(&filaEntrada.conteudo[0], 0, sizeof(Mensagem));

    //faz voltar 1 pos
    for(int i = 1; i<filaEntrada.tamanho;i++){
        filaEntrada.conteudo[i-1] = filaEntrada.conteudo[i];
    }

    //diminui o tamanho
    filaEntrada.tamanho --;

    //libera o lock e tira do semafaro, semafaro tem q ter coisa

    //o semaforo só muda na thead
    //sem_wait(&filaEntrada.cheio);

    pthread_mutex_unlock(&filaEntrada.lock);

    return retorno;
    

}



//pega o topo da pilha da fila de saida
Mensagem getMsgFilaSaida(){
    
    //msg de erro, acho q pode dar erro, acho estranho como ta mas tava funcionando
    Mensagem vazio = {0};

    //tenta pegar o lock
    pthread_mutex_lock(&filaSaida.lock);


    //get lock
    if(filaSaida.tamanho == 0){

        pthread_mutex_unlock(&filaSaida.lock);
        return vazio;

    }

    //passou os testes, pega a mensagem

    Mensagem retorno = filaSaida.conteudo[0];
    
    //tira da lista zera os bytes
    memset(&filaSaida.conteudo[0], 0, sizeof(Mensagem));

    //faz voltar 1 pos
    for(int i = 1; i<filaSaida.tamanho;i++){
        filaSaida.conteudo[i-1] = filaSaida.conteudo[i];
    }

    //diminui o tamanho
    filaSaida.tamanho --;

    //libera o lock e tira do semafaro, semafaro tem q ter coisa

    pthread_mutex_unlock(&filaSaida.lock);

    return retorno;
    

}



void printMsg(Mensagem msg){

      char *tipo;
    if (msg.tipo == Controle){
        tipo = "Controle";
    }
    else{
        tipo = "dado";
    }

    printf("printando Mensagem:\n");
    printf("Origem: %d\nDestino: %d\nTipo: %s\nConteudo: %s",msg.origem, msg.destino,tipo,msg.conteudo);


}


//pra imprimir msg d texto
void printMsgFormatada(Mensagem msg){

    printf("Nova Mensagem do roteador: %d :\n", msg.origem);
    printf("%s",msg.conteudo);


}




void printFila(Fila fila){

    //se der pau botar o lock
    pthread_mutex_lock(&fila.lock);
    for(int i =0; i<fila.tamanho;i++){
        printf("Mensagem na posicao %d: %s \n", i, fila.conteudo[i].conteudo);
    }
    pthread_mutex_unlock(&fila.lock);
}


//func para criar msg de dado
Mensagem criarMsgDados(){

    Mensagem newMsg;

    newMsg.origem = id;
    
    //o define n estava fncionando
    newMsg.tipo = Dado;


    int escolha;

    while(1){
        printf("Digite o Destino da Mensagem: \n");

        //faz um teste para ver se leu um valor inteiro, caso contrario usuario foi burro
        if (scanf("%d", &escolha) != 1) {
            printf("Entrada invalida!\n");
            //limpa o buffer do scanf
            while (getchar() != '\n');
            continue;
        }
        //isso é para debug tirar o + 1000
        if(escolha == id + 1000){
            printf("Destino igual a origem, escolha novamente\n"); 
            //limpa o buffer do scanf
            while (getchar() != '\n');
            continue;
        }
       

        newMsg.destino = escolha;
        while (getchar() != '\n'); //limpa o ENTER que sobra do scanf
        break;
    }

    printf("Digite o conteudo da mensagem: \n");
    while(1){

        //pega a msg e salva
        fgets(newMsg.conteudo, sizeof(newMsg.conteudo), stdin);

        //tira o '\n' que o fgets pode deixar no final
        newMsg.conteudo[strcspn(newMsg.conteudo, "\n")] = '\0';

        //se for maior q 100 manda fazer dnv
        if(strlen(newMsg.conteudo) > 100){

            printf("Mensagem muito grande, tente novamente\n");
            while (getchar() != '\n');
            continue;
        }

        break;
    }    

    return newMsg;
}




//abr o arquivo e pega o scokt
int pegaSocket(const char *filename, int idProcurar) {

    //printf("chegou o numero : %d\n", idProcurar);

    FILE *f = fopen(filename, "r");
    if (!f) {
        printf("deu pao, n abriu o arquivo %s\n", filename);
        return -1;
    }

    int tempId, port;
    char ip[64];

    // olha cada linha no formato "id port ip", o 63 é para pegar o ip, mas vai ignorar se n for local
    while (fscanf(f, "%d %d %63s", &tempId, &port, ip) == 3) {
        if (tempId == idProcurar) {
            fclose(f);


            printf("retornando %d para o id: %d\n", port,idProcurar);

            return port; //achou a porta
        }
    }




    fclose(f);
    return -1; // não achou o id
}





void zerarFilaTesta(){

    //marca como todos analizados pois n veio nenhum ainda
    pthread_mutex_lock(&vetoresParaAnalise.lock);

    for (int i = 0; i < numRoteadores; i++)
        vetoresParaAnalise.testados[i] = 1;

    pthread_mutex_unlock(&vetoresParaAnalise.lock);

}


//vai botar o vetor para analizar, passa um vetor distancia, e o roteador q mandou
void addVetorAnalize(int roteadorOrigem, vetoresRecebidos vetorAdicionar){

    pthread_mutex_lock(&vetoresParaAnalise.lock);

    //bota o no lugar do roteador q mandou
    vetoresParaAnalise.vetoresNaoAnalizados[roteadorOrigem-1] = vetorAdicionar;

    //marca como nao analizado
    vetoresParaAnalise.testados[roteadorOrigem-1] = 0;

    pthread_mutex_unlock(&vetoresParaAnalise.lock);


}




//só chamar com o lock obtido, só pra debug
void imprimirVetorDistancia(){

    time_t agora;
    struct tm *infoTempo;

    time(&agora);                     //pega o tempo atual
    infoTempo = localtime(&agora);    //converte para horário local

    char buffer[80];
    strftime(buffer, sizeof(buffer), "%H:%M:%S", infoTempo);  //formata como HH:MM:SS

    printf("Hora atual: %s\n", buffer);

    for(int i =0; i< numRoteadores;i++){
        printf("\nRoteador %d que esta na pos %d do vetor:\n", i+1, i);
        printf("vizinho : %d, custo: %d\n", vetorDistancia.vetores[i].isVisinho,vetorDistancia.vetores[i].custo );
        printf("saida: %d, tempo sem mandar o vetor: %d\n\n",vetorDistancia.vetores[i].saida, vetorDistancia.vetores[i].rodadasSemResposta);
    
        
    }

}





//theads ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void *theadFilaEntrada() {

    //printFila(filaEntrada);
   

    while (roteadorInciado) {
        int result = sem_trywait(&filaEntrada.cheio);
        
        if (result == -1) {
            usleep(100000);
            continue;
        }

        Mensagem newMensagem = getMsg();


        //msg pra printar pta mim
        if(newMensagem.tipo == Dado && newMensagem.destino == id){
            printMsgFormatada(newMensagem);
        }

        if (newMensagem.tipo == Controle && newMensagem.destino == id){

            //formatar a msg para passar nesse vetor
            vetoresRecebidos newVetor;

            //vai receber o custo para todos em ordem
            //cuida da string recebida da msg

            
            char msg[500];
            
            strncpy(msg, newMensagem.conteudo, sizeof(msg) - 1);
            msg[sizeof(msg) - 1] = '\0'; //garante terminação

            //contador
            int i = 0;

            // quebra a string por espaços
            char *token = strtok(msg, " ");

            //olhar onde devo colocar 
            while (token != NULL && i < numRoteadores) {
                newVetor.vetores[i].custo = atoi(token);  // converte pra int
                newVetor.vetores[i].destino = i + 1; //pq o roteador 1 fica na pos 0
                i++;
                token = strtok(NULL, " ");
            }

            

            //////////////////fazeeeeeeeeeeeeeeeeeeeeeeeeeeer

            addVetorAnalize(newMensagem.origem,newVetor);

        }
    
        if(newMensagem.destino != id && newMensagem.tipo == Dado){
            sendMsg(newMensagem);
        }
    }
    return NULL;
}


// a função para enviar via FIFO
int enviarViaFIFO(int idDestino, Mensagem msg) {
    char fifoName[64];
    snprintf(fifoName, sizeof(fifoName), "fifo_roteador_%d", idDestino);

    int fd = open(fifoName, O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        return -1;
    }

    char buffer[600];
    snprintf(buffer, sizeof(buffer), "%d|%d|%d|%s", 
             msg.tipo, msg.origem, msg.destino, msg.conteudo);

    write(fd, buffer, strlen(buffer) + 1);
    close(fd);
    return 0;
}

// Thread receptora
void *theadReceptorFIFO() {
    char buffer[600];

    while (roteadorInciado) {
        if (globalFifoFd < 0) {
            usleep(100000);
            continue;
        }

        int bytesLidos = read(globalFifoFd, buffer, sizeof(buffer) - 1);

        if (bytesLidos > 0) {
            buffer[bytesLidos] = '\0';

            Mensagem novaMensagem = {0};
            char conteudo[500];
            
            if (sscanf(buffer, "%d|%d|%d|%499[^\n]", 
                      &novaMensagem.tipo, 
                      &novaMensagem.origem, 
                      &novaMensagem.destino, 
                      conteudo) == 4) {
                
                strncpy(novaMensagem.conteudo, conteudo, sizeof(novaMensagem.conteudo) - 1);
                novaMensagem.conteudo[sizeof(novaMensagem.conteudo) - 1] = '\0';
                
                addMsg(novaMensagem);
            }
        } else {
            usleep(100000);
        }
    }
    return NULL;
}

// CORRIJA theadFSaida
void *theadFSaida() {
    while (roteadorInciado) {
        int result = sem_trywait(&filaSaida.cheio);
        
        if (result == -1) {
            usleep(100000);
            continue;
        }

        Mensagem newMsg = getMsgFilaSaida();

        if(newMsg.destino == id){
            continue;
        }
        
        pthread_mutex_lock(&vetorDistancia.lock);
        int saida = vetorDistancia.vetores[newMsg.destino - 1].saida;
        pthread_mutex_unlock(&vetorDistancia.lock);

        if (saida <= 0) {
            continue;
        }

        enviarViaFIFO(saida, newMsg);
    }
    return NULL;
}

// cuida dos vetores distancias
void *theadVetorDistancia(){

    zerarFilaTesta();
    pthread_mutex_lock(&vetorDistancia.lock);
   
    for(int i = 0; i < numRoteadores; i++){
        if (i == id - 1){
            vetorDistancia.vetores[i].custo = 0;
            vetorDistancia.vetores[i].saida = id;
            vetorDistancia.vetores[i].isVisinho = 0;
            vetorDistancia.vetores[i].rodadasSemResposta = 0;
        }
        else{
            vetorDistancia.vetores[i].custo = -1;
            vetorDistancia.vetores[i].saida = -1;
            vetorDistancia.vetores[i].isVisinho = 0;
            vetorDistancia.vetores[i].rodadasSemResposta = 0;
        }
    }

    FILE *f = fopen(ENLACE_FILE, "r");
    if (!f) {
        printf("Erro ao abrir o arquivo.\n");
        pthread_mutex_unlock(&vetorDistancia.lock);
        return NULL;
    }

    int id1, id2, custo;
    int vizinho;

    while (fscanf(f, "%d %d %d", &id1, &id2, &custo) == 3) {
        if (id1 == id) vizinho = id2;
        if (id2 == id) vizinho = id1;

        if (id == id1 || id == id2) {
            vetorDistancia.vetores[vizinho - 1].custo = custo;
            vetorDistancia.vetores[vizinho - 1].isVisinho = 1;
            vetorDistancia.vetores[vizinho - 1].saida = vizinho;
        }
    }
    fclose(f);

    if (debugando == 1) {
        imprimirVetorDistancia();
    }

    pthread_mutex_unlock(&vetorDistancia.lock);

    if(debugando == 1){
        printf("Iniciando o envio dos vetores distancia\n");
    }

    // Aguarda 10 segundos COM verificação
    for(int i = 0; i < 10 && roteadorInciado; i++) {
        sleep(1);
    }

    int iteracoes = 0;
    int maxIteracoes = 15; // ADICIONE: máximo de iterações (30 segundos com sleep 2)

    // Loop com CONDIÇÃO DE PARADA
    while (roteadorInciado && iteracoes < maxIteracoes) {
        iteracoes++;

        pthread_mutex_lock(&vetorDistancia.lock);

        int mudou = 0;
        int vizinhosAtivos = 0; // ADICIONE: contador de vizinhos ativos

        for (int i = 0; i < numRoteadores; i++) {
            if (vetorDistancia.vetores[i].isVisinho == 1) {
                vizinhosAtivos++; // ADICIONE: conta vizinhos ainda ativos
                vetorDistancia.vetores[i].rodadasSemResposta++;
            }
        }

        pthread_mutex_lock(&vetoresParaAnalise.lock);

        for (int i = 0; i < numRoteadores; i++) {
            if (vetoresParaAnalise.testados[i] == 0) {
                for (int d = 0; d < numRoteadores; d++) {
                    int destino = vetoresParaAnalise.vetoresNaoAnalizados[i].vetores[d].destino;
                    int custoRecebido = vetoresParaAnalise.vetoresNaoAnalizados[i].vetores[d].custo;

                    if (destino <= 0 || custoRecebido < 0)
                        continue;

                    int custoAteVizinho = vetorDistancia.vetores[i].custo;
                    vetorDistancia.vetores[i].rodadasSemResposta = 0;

                    if (custoAteVizinho < 0)
                        continue;

                    int novoCusto = custoAteVizinho + custoRecebido;

                    if (novoCusto > 32)
                        continue;

                    if (vetorDistancia.vetores[destino - 1].custo < 0 ||
                        novoCusto < vetorDistancia.vetores[destino - 1].custo) {
                        vetorDistancia.vetores[destino - 1].custo = novoCusto;
                        vetorDistancia.vetores[destino - 1].saida = i + 1;
                        mudou = 1;
                    }
                }
                vetoresParaAnalise.testados[i] = 1;
            }
        }

        for (int i = 0; i < numRoteadores; i++) {
            if (vetorDistancia.vetores[i].isVisinho == 1 &&
                vetorDistancia.vetores[i].rodadasSemResposta >= 3) {
                vetorDistancia.vetores[i].isVisinho = 0;
                vetorDistancia.vetores[i].custo = -1;
                vetorDistancia.vetores[i].saida = -1;
                mudou = 1;
                vizinhosAtivos--; // ADICIONE: decrementa vizinhos ativos

                if(debugando == 1){
                    printf("Enlace com o roteador %d caiu!\n", i+1);
                }
            }
        }

        // ADICIONE: Se não há mais vizinhos, sai do loop
        if (vizinhosAtivos == 0) {
            if(debugando == 1){
                printf("Todos os enlaces caíram. Finalizando thread de roteamento.\n");
            }
            pthread_mutex_unlock(&vetoresParaAnalise.lock);
            pthread_mutex_unlock(&vetorDistancia.lock);
            break; // SAIA DO LOOP
        }

        int anyZero = 0;
        for (int x = 0; x < numRoteadores; x++) {
            if (vetoresParaAnalise.testados[x] == 0) { 
                anyZero = 1; 
                break; 
            }
        }
        if (!anyZero) {
            for (int x = 0; x < numRoteadores; x++)
                vetoresParaAnalise.testados[x] = 0;
        }

        pthread_mutex_unlock(&vetoresParaAnalise.lock);

        char stringControle[500] = "";
        char temp[32];

        for (int aux = 0; aux < numRoteadores; aux++) {
            snprintf(temp, sizeof(temp), "%d ", vetorDistancia.vetores[aux].custo);
            if (strlen(stringControle) + strlen(temp) < sizeof(stringControle) - 1) {
                strcat(stringControle, temp);
            }
        }

        if (mudou == 1) {
            for (int roteadores = 0; roteadores < numRoteadores; roteadores++) {
                if (vetorDistancia.vetores[roteadores].isVisinho == 1) {
                    Mensagem novaMsgControle;
                    novaMsgControle.origem = id;
                    novaMsgControle.destino = roteadores + 1; 
                    novaMsgControle.tipo = Controle;
                    strncpy(novaMsgControle.conteudo, stringControle, sizeof(novaMsgControle.conteudo) - 1);
                    novaMsgControle.conteudo[sizeof(novaMsgControle.conteudo) - 1] = '\0';

                    sendMsg(novaMsgControle);
                }
            }
        }

        pthread_mutex_unlock(&vetorDistancia.lock);
        sleep(2);
    }

    // garante que o estado global reflita que o roteador parou
    pthread_mutex_lock(&vetorDistancia.lock);
    roteadorInciado = 0;
    pthread_mutex_unlock(&vetorDistancia.lock);

    // sinaliza para o main que a thread terminou
    threadTerminated = 1;    // ADICIONE ISTO

    printf("Thread de roteamento finalizada.\n");
    fflush(stdout);
    return NULL;
}

// watcher para aguardar o fim da thread de roteamento
void *vetorWatcher(void *arg) {
    pthread_t tid = *(pthread_t*)arg;
    free(arg);
    pthread_join(tid, NULL);

    // sinaliza que a thread terminou
    vetorThreadRunning = 0; // vetorThreadRunning deve ser global (int)
    printf("Watcher: thread de roteamento finalizou.\n");
    fflush(stdout);
    return NULL;
}

//terminal é a main
int main(int argc, char *argv[])
{
    // ADICIONE ESTA VERIFICAÇÃO
    if (argc < 2) {
        printf("Uso: %s <id_roteador>\n", argv[0]);
        printf("Exemplo: %s 1\n", argv[0]);
        return 1;
    }

    id = atoi(argv[1]); //converte o argumento para inteiro
    
    meuSocket = pegaSocket("roteador.config", id);

    if (meuSocket == -1) {
        printf("Erro: Roteador %d não encontrado\n", id);
        return 1;
    }

    printf("socket: %d\n", meuSocket);

    // Cria e abre FIFO
    char fifoName[64];
    snprintf(fifoName, sizeof(fifoName), "fifo_roteador_%d", id);
    unlink(fifoName);
    mkfifo(fifoName, 0666);
    
    globalFifoFd = open(fifoName, O_RDONLY | O_NONBLOCK);
    if (globalFifoFd < 0) {
        perror("Erro ao abrir FIFO");
        return 1;
    }

    initFilas();
    pthread_mutex_init(&vetorDistancia.lock, NULL);
    pthread_mutex_init(&vetoresParaAnalise.lock, NULL);

    pthread_t tEntrada, tSaida, tVetorDistancia, tReceptor;
    // usa a variável global vetorThreadRunning (removida a declaração local)

    pthread_create(&tEntrada, NULL, theadFilaEntrada, NULL);
    pthread_create(&tSaida, NULL, theadFSaida, NULL);
    pthread_create(&tReceptor, NULL, theadReceptorFIFO, NULL);
   
    int escolha;
    int menuShown = 0;
    char buf[64];

    while (1) {
        /* notifica fim da thread e força reimpressao do menu */
        if (threadTerminated) {
            printf("\n[MAIN] Thread de roteamento terminou — voltando ao menu.\n");
            fflush(stdout);
            threadTerminated = 0;
            menuShown = 0;
        }

        /* imprime o menu apenas se ainda não estiver mostrado */
        if (!menuShown) {
            printf("\n===============================\n");
            printf(" Bem-vindo a interface do roteador: %d\n", id);
            printf("===============================\n");
            printf("1 - Iniciar Roteador\n");
            printf("2 - Enviar Mensagem\n");
            printf("3 - Ver Tabela de Roteamento\n");
            printf("4 - DEBUG\n");
            printf("0 - Sair\n");
            printf("-------------------------------\n");
            printf("Digite o numero da opcao desejada: ");
            fflush(stdout);
            menuShown = 1;
        }

        /* espera stdin com timeout para checar flags periodicamente */
        fd_set rfds;
        struct timeval tv;
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int sel = select(STDIN_FILENO + 1, &rfds, NULL, NULL, &tv);
        if (sel <= 0) {
            /* timeout ou erro: volta ao topo para rechecar threadTerminated */
            continue;
        }

        /* há entrada do usuário */
        if (FD_ISSET(STDIN_FILENO, &rfds)) {
            if (fgets(buf, sizeof(buf), stdin) == NULL) {
                clearerr(stdin);
                continue;
            }
            escolha = (int)strtol(buf, NULL, 10);
        } else {
            continue;
        }

        printf("-------------------------------\n");

        switch (escolha) {
            case 1:
                if (roteadorInciado == 1) {
                    printf("Roteador ja iniciado\n");
                } else {
                    roteadorInciado = 1;
                    printf("Iniciando Roteador.\n");
                    pthread_create(&tVetorDistancia, NULL, theadVetorDistancia, NULL);
                    vetorThreadRunning = 1;

                    pthread_t tWatcher;
                    pthread_t *pt = malloc(sizeof(pthread_t));
                    *pt = tVetorDistancia;
                    pthread_create(&tWatcher, NULL, vetorWatcher, pt);
                }
                menuShown = 0; // voltar a mostrar o menu
                break;

            case 2:
                if (roteadorInciado == 0) {
                    printf("Inicie o roteador primeiro!\n");
                } else {
                    printf("Enviando Mensagem\n");
                    Mensagem novaMensagem = criarMsgDados();
                    sendMsg(novaMensagem);
                }
                menuShown = 0;
                break;

            case 3:
                if (roteadorInciado == 0) {
                    printf("Inicie o roteador primeiro!\n");
                } else {
                    pthread_mutex_lock(&vetorDistancia.lock);
                    imprimirVetorDistancia();
                    pthread_mutex_unlock(&vetorDistancia.lock);
                }
                menuShown = 0;
                break;

            case 4:
                printf("DEBUG: Tamanho fila entrada: %d\n", filaEntrada.tamanho);
                menuShown = 0;
                break;

            case 0:
                printf("Saindo...\n");
                roteadorInciado = 0;
                sleep(1);
                close(globalFifoFd);
                unlink(fifoName);
                return 0;

            default:
                printf("Opção inválida!\n");
                menuShown = 0;
                break;
        }
    }
    return 0;
}
