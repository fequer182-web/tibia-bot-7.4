#pragma once

namespace completion_guidance {
struct Guide {std::string title,meaning,a,b;};
inline Guide forMethod(const std::string& key) {
    static const std::map<std::string,Guide> guides{
        {"gameAutoWalk",{"Andar pelo clique","Solicita um caminho ate o destino.","Clique num destino livre a poucos SQMs; aguarde o personagem chegar.","Clique em outro destino livre, numa direcao diferente; aguarde chegar."}},
        {"localPlayerAutoWalk",{"Percorrer o caminho","Executa o caminho do personagem local.","Clique num destino livre a poucos SQMs; aguarde chegar.","Clique em outro destino livre numa direcao diferente; aguarde chegar."}},
        {"gameAttack",{"Atacar","Define ou cancela o alvo de ataque.","Ataque uma criatura que voce possa atacar normalmente.","Cancele o ataque e ataque outra criatura."}},
        {"gameFollow",{"Seguir","Define ou cancela a criatura seguida.","Use Seguir em uma criatura.","Cancele e use Seguir em outra criatura."}},
        {"gameLook",{"Olhar objeto","Envia a acao Look no objeto escolhido.","De Look em um item conhecido e confira a mensagem.","De Look em outro item, diferente do primeiro."}},
        {"gameDetailedLook",{"Olhar objeto - alternativa","Outra entrada candidata para a acao Look desta build.","De Look em um item conhecido e confira a mensagem.","De Look em outro item, diferente do primeiro."}},
        {"gameMove",{"Mover item","Move um item para outro lugar.","Arraste um item entre dois SQMs ou da bag para outro slot.","Mova outro item entre lugares diferentes."}},
        {"gameUse",{"Usar objeto","Executa Use em um objeto.","Use uma bag fechada para abri-la.","Use outra bag fechada para abri-la."}},
        {"gameUseWith",{"Usar ferramenta no alvo","Usa um item sobre outro objeto.","Use uma ferramenta que tenha Use With em um alvo valido.","Repita com outro alvo valido para essa ferramenta."}},
        {"gameTalk",{"Falar","Entrada candidata para enviar uma mensagem.","Escreva e envie ABI_TEST_ALPHA no chat normal.","Escreva e envie ABI_TEST_BETA no mesmo chat."}},
        {"gameTalkChannel",{"Falar no canal","Entrada candidata para enviar mensagem no canal selecionado.","Escreva e envie ABI_TEST_ALPHA no canal que voce usa.","Escreva e envie ABI_TEST_BETA no mesmo canal."}},
        {"mapGetTile",{"Consultar SQM do mapa","Busca o tile: o quadrado do mapa, nao um item especifico.","De Look em um item no chao de um SQM proximo; clique tambem num destino livre proximo.","Repita em outro SQM, com outro item e outro destino livre."}},
        {"mapGetCreatureById",{"Buscar criatura pelo ID","Localiza uma criatura conhecida pelo cliente.","Selecione uma criatura na battle list e de Look nela.","Selecione outra criatura na battle list e de Look nela."}},
        {"mapGetCreatureByName",{"Buscar criatura pelo nome","Localiza uma criatura pelo nome; nem toda interface usa esta funcao.","Use Seguir em uma criatura conhecida, pela lista/interface do cliente.","Use Seguir em outra criatura de nome diferente."}},
        {"tileGetThings",{"Objetos de um SQM","Consulta os objetos presentes no quadrado do mapa.","De Look em um item no chao e comece arrasta-lo; cancele o arraste.","Repita em outro SQM com outro item no chao."}},
        {"tileGetThingCount",{"Quantidade de objetos no SQM","Consulta quantos objetos existem no quadrado do mapa.","De Look em um item de um SQM com poucos objetos.","De Look em outro SQM com uma quantidade diferente de objetos."}},
        {"tileGetTopMoveThing",{"Bloqueador para mover","Escolhe o objeto que o cliente permite mover naquele SQM.","Comece arrastar um item do chao; cancele sem move-lo.","Repita com outro item em outro SQM."}},
        {"tileGetTopUseThing",{"Objeto para usar no SQM","Escolhe o objeto do chao para a acao Use.","Use um objeto do chao que realmente tenha Use.","Use outro objeto do chao em outro SQM."}},
        {"tileGetGround",{"Chao do SQM","Consulta o objeto que representa o piso do quadrado.","De Look ou clique para andar em um SQM de piso conhecido.","Repita em outro SQM com piso diferente."}},
        {"tileIsWalkable",{"SQM onde pode andar","Consulta se o personagem pode entrar naquele quadrado.","Clique para andar ate um SQM livre e proximo.","Clique em outro SQM livre, por outro caminho."}},
        {"tileIsPathable",{"SQM para calcular caminho","Consulta se o quadrado pode participar do caminho.","Clique num destino livre que exija andar varios SQMs.","Clique em outro destino livre numa direcao diferente."}},
        {"localPlayerGetInventoryItem",{"Item equipado","Consulta um slot do equipamento do personagem.","De Look em um item equipado, por exemplo na mao.","De Look em outro item equipado, por exemplo nos pes."}},
        {"itemGetId",{"ID do item","Le o identificador do item; nao e a posicao na pilha.","De Look em um item conhecido.","De Look em outro tipo de item, com ID diferente."}},
        {"itemGetCount",{"Quantidade do item","Le a quantidade de uma pilha.","De Look em uma pilha de itens.","De Look em outra pilha com quantidade diferente."}},
        {"containerGetItem",{"Item dentro da bag","Consulta um item em um slot da bag.","Abra uma bag e de Look em um item dentro dela.","Abra outra bag e de Look em um item em outro slot."}},
        {"containerGetItems",{"Lista de itens da bag","Consulta os itens de uma bag aberta.","Abra uma bag com itens; mova um item entre seus slots.","Abra outra bag e mova um item entre slots dela."}},
        {"gameGetContainers",{"Bags abertas","Consulta a lista de bags abertas; nao abre uma bag por si so.","Abra uma bag e depois feche a janela dela.","Abra duas bags e depois feche uma delas."}},
        {"thingGetPosition",{"Posicao de um objeto","Consulta as coordenadas de um objeto do jogo.","De Look em um item no chao de um SQM conhecido.","De Look em outro item no chao de outro SQM."}},
        {"containerGetCapacity",{"Numero de slots da bag","Consulta quantos slots a bag comporta. Nao e a Capacity de peso do personagem.","Abra uma bag e mantenha a janela aberta para o cliente desenhar os slots.","Abra outra bag, de preferencia com numero de slots diferente."}}
    };
    const auto found=guides.find(key);
    if(found!=guides.end())return found->second;
    return {key,"Sem roteiro especifico: este teste deve permanecer pendente.","Nao execute acoes por tentativa; pule este teste.","Pule este teste ate existir um roteiro especifico."};
}
inline std::string prompt(const std::string& key,bool second) {
    const auto guide=forMethod(key);
    return std::string(second?"ACAO B: ":"ACAO A: ")+(second?guide.b:guide.a)+" Depois volte ao console e pressione ENTER...";
}
inline void explain(const std::string& key,size_t candidates) {
    const auto guide=forMethod(key);
    std::cout<<"\nTESTE: "<<guide.title<<" ("<<key<<")\nO que faz: "<<guide.meaning
        <<"\nA: "<<guide.a<<"\nB: "<<guide.b
        <<"\nA e B sao duas amostras da mesma funcao. Execute no jogo, depois volte para ENTER.\n"
        <<"As acoes sao estimulos para observar; nao garantem que esta entrada sera chamada.\n";
    if(candidates>1)std::cout<<"Ha "<<candidates<<" enderecos candidatos. Repita o MESMO roteiro em cada um; o Builder compara as evidencias. Voce nao precisa escolher qual e correto.\n";
    std::cout<<"Sem captura significa nao observado; nao confirma nem descarta a funcao. Pode pular se nao conseguir realizar a acao.\n";
}
}
