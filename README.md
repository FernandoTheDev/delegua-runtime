# Delegua Runtime

Este repositório contém o ambiente de execução (*runtime*) da linguagem de programação **Delégua**, distribuído sob a licença **Apache 2.0**.

O projeto tem como alvo os *backends* de baixo nível da linguagem (como LLVM e Assembly x86_64). Ele reimplementa em nível nativo os recursos presentes na versão interpretada da linguagem, incluindo funções *builtin*, métodos nativos de tipos primitivos e um coletor de lixo (*Garbage Collector*) para gerenciamento automático de memória.

## Compilação e testes

Requer compilador C99 (Clang ou GCC), `pkg-config` e Boehm GC (`bdw-gc`).

```sh
./build
cc exemplos/9.c libdelegua_rt.o $(pkg-config --cflags --libs bdw-gc) -rdynamic -ldl -lm -o exemplo
./tests/run.sh
CFLAGS='-DNDEBUG -fsanitize=address,undefined -fno-omit-frame-pointer' ./tests/run.sh
```

`./build -ll` gera LLVM IR com Clang; `./build -s` gera assembly. Esses formatos
ficam em `.build/`, um arquivo por módulo. O modo padrão continua produzindo
`libdelegua_rt.o`, incluindo o ponto de entrada que chama `delegua_main()`.
Para aplicações com `main()` próprio, vincule os módulos exceto `delegua_rt.c`.

A implementação está dividida em valores e conversões (`valores.c`), operadores
(`operadores.c`), vetores/tuplas/mapas (`colecoes.c`), classes (`objetos.c`),
saída (`saida.c`), FFI (`ffi.c`) e entrada do programa (`delegua_rt.c`).
O contrato público está em `src/delegua_rt.h`; `interno.h` contém auxiliares privados.

## Coleções

`create_nulo()` representa nulo. `create_tupla(count, values)` copia os elementos
para uma tupla imutável; obter e fatiar funcionam tanto em vetores quanto tuplas.
Índices negativos contam a partir do fim. `delegua_slice(valor, início, fim)`
usa intervalo com fim exclusivo e rejeita limites inválidos ou invertidos.
Textos são contados e fatiados em bytes; não há indexação por caracteres Unicode.
`delegua_tamanho()` retorna o tamanho de textos, vetores, tuplas e mapas.

```c
delegua_value mapa = create_map();
delegua_map_setar(mapa, create_text("nome"), create_text("Ana"));
delegua_value nome = delegua_map_obter(mapa, create_text("nome"));
bool existe = delegua_map_contem(mapa, create_text("nome"));
delegua_map_remover(mapa, create_text("nome"));
```

Mapas usam hash com resolução de colisões por encadeamento e crescimento
automático. Chaves aceitas: nulo, lógico, inteiro, real finito e texto (incluindo
bytes nulos). Inteiros e reais de mesmo valor inteiro compartilham a chave;
textos e lógicos permanecem distintos dos números. Busca ausente retorna nulo;
`contem` distingue ausência de um valor nulo armazenado. Coleções guardam
referências aos valores e são gerenciadas pelo GC.

## Classes e funções

```c
static delegua_value inicializar(sz argc, delegua_value *argv) {
    (void)argc;
    return delegua_objeto_setar(argv[0], "nome", argv[1]);
}

/* Dentro de delegua_main(): */
delegua_value pessoa = create_classe("Pessoa", create_nulo());
delegua_classe_metodo(pessoa, "construtor", create_funcao(inicializar, 1));
delegua_value argumento = create_text("Ana");
delegua_value instancia = delegua_instanciar(pessoa, 1, &argumento);
delegua_value nome = delegua_objeto_obter(instancia, "nome");
```

O segundo argumento de `create_classe` pode ser outra classe para herança
simples. Métodos são procurados na classe e depois nas superclasses; registrar
um método na subclasse permite sobrescrita. Campos pertencem a cada instância
e têm precedência na consulta. Métodos obtidos por `delegua_objeto_obter()`
ficam ligados à instância e podem ser chamados com `delegua_chamar()`.
A aridade de `create_funcao()` conta apenas os argumentos explícitos; callbacks
de métodos recebem a instância em `argv[0]` e `argc` inclui esse receptor.
O método `construtor`, inclusive herdado, é chamado na instanciação.
Funções livres usam o mesmo callback sem receptor. Aridade é fixa.

## Validação e limites

Erros de índice, tipos, alocação, overflow inteiro, deslocamentos inválidos e
conversões numéricas inválidas encerram o processo com diagnóstico em stderr.
As validações continuam ativas com `NDEBUG`. Conversão de texto numérico exige
consumir todo o conteúdo. Deslocamento à esquerda usa os 64 bits sem overflow
assinado. A saída de coleções limita a profundidade para suportar ciclos.

FFI carrega código nativo confiável e exige funções com assinatura
`delegua_native_fn`. Os handles agora têm tipo `DELEGUA_T_BIBLIOTECA`;
ponteiros arbitrários não são handles válidos. Fechar invalida todas as cópias
do handle. Bibliotecas devem ser fechadas explicitamente. FFI não oferece
isolamento de código nativo. O runtime e seus mapas não oferecem sincronização
para acesso concorrente.

Este runtime fornece primitivas para backends: não inclui parser, compilador,
tratamento de exceções, módulos da biblioteca padrão completa ou despacho de
`super` dedicado. Mapas, classes e funções são impressos como identificadores
de tipo (`<dicionário>`, `<classe>`, etc.).
