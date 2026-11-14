# Runtime nativo de Delégua

Primitivas C99 usadas pelo CGD: valores gerenciados por Boehm GC, operações,
coleções, classes, funções, módulos fonte compilados e exceções. Licença Apache-2.0.
A API pública está em `src/delegua_rt.h`; `interno.h` e os módulos C implementam
os detalhes. A linguagem compilada não expõe todos os pontos de entrada da API C.

## Build e testes

Requer Clang ou GCC, pkg-config e Boehm GC com headers (`bdw-gc`).

```sh
./build
CC=gcc ./build
./tests/run.sh

CFLAGS='-DNDEBUG -fsanitize=address,undefined -fno-omit-frame-pointer' \
  ASAN_OPTIONS=detect_leaks=0:detect_stack_use_after_return=0 \
  UBSAN_OPTIONS=halt_on_error=1 ./tests/run.sh
```

`./build -ll` gera LLVM IR com Clang, e `./build -s` gera assembly. O modo padrão
combina os módulos em `libdelegua_rt.o`, incluindo o `main()` que chama
`delegua_main()`. Para um programa C com main próprio, vincule os módulos exceto
`delegua_rt.c`. Para o objeto padrão:

```sh
cc exemplos/9.c libdelegua_rt.o $(pkg-config --cflags --libs bdw-gc) \
  -rdynamic -ldl -lm -o exemplo
```

ASan precisa usar a pilha real para o GC conservativo enxergar suas raízes;
por isso `detect_stack_use_after_return=0`. O detector de vazamentos também é
desabilitado para memória administrada pelo GC. ASan e UBSan continuam ativos.
A suíte completa do compilador em `scripts/sanitizadores.py` também instrumenta
e executa os programas gerados, incluindo todos os exemplos.

## Organização

| Módulo | Responsabilidade |
| --- | --- |
| `valores.c` | Nomes de tipos, conversões e posição de erro |
| `operadores.c`, `adicionais.c` | Aritmética, bits, comparação, potência e pertencimento |
| `colecoes.c` | Vetores, tuplas, mapas, índices, fatias, expansão e clonagem |
| `objetos.c` | Classes, herança, visibilidade, acessores, funções e descritores de tipos |
| `excecoes.c` | Frames de tratamento, Excecao, falhar e erro do runtime |
| `biblioteca.c`, `nativas.c` | Funções globais, métodos nativos, entrada e conversão para texto |
| `textos.c`, `unicode_tables.h` | UTF-8 e transformações Unicode sem locale global |
| `modulos.c` | Tabela protegida de exportações de módulos compilados |
| `saida.c` | Saída dos valores |
| `ffi.c` | API C para bibliotecas nativas confiáveis, não exposta pelo CGD |

## Valores e coleções

`delegua_value` possui etiqueta de tipo e dados. Não misture membros da union
sem verificar a etiqueta. `create_nulo`, `create_num`, `create_real`,
`create_bool`, `create_text_len`, `create_vetor`, `create_tupla` e `create_map`
criam os valores correspondentes. Textos preservam bytes nulos internos.

`delegua_obter` e `delegua_setar` despacham indexação validada. Índices negativos
contam a partir do fim. Textos são indexados e contados por pontos de código
UTF-8; tuplas não aceitam escrita. `delegua_intervalo` implementa fatias com
passo, limites omitidos e ajuste ao intervalo válido. `delegua_slice` conserva
a API de limites explícitos e estritos.

Mapas usam hash e encadeamento separado. As chaves podem ser nulo, lógico,
inteiro, real finito ou texto. Busca ausente retorna nulo. Inteiros/reais de mesmo
valor inteiro compartilham chave; lógicos/textos não são números. A ordem de
iteração não é garantida. `delegua_iterar` retorna pares imutáveis para mapas.

Clonagem profunda preserva ciclos e compartilhamento interno. Classes e funções
mantêm suas identidades; instâncias conservam a classe e clonam seus campos.
Recursos nativos arbitrários não são clonados.

## Funções e classes

`create_funcao(fn, aridade)` cria uma função de aridade fixa;
`create_funcao_intervalo(fn, minimo, maximo)` aceita padrões e parâmetros
variáveis. A ABI é `delegua_value fn(sz argc, delegua_value *argv)`.
`delegua_chamar` valida a aridade antes de chamar. Funções sem ambiente capturado
e métodos com receptor explícito usam a mesma ABI.

Métodos ligados recebem a instância em `argv[0]`; a aridade pública exclui esse
receptor. Construtores, inclusive herdados, são chamados na instanciação.
`delegua_super_metodo` resolve a partir da superclasse lexical.

Classes compiladas são fechadas: campos devem ser declarados. Os acessos gerados
usam `delegua_propriedade_em` e `delegua_definir_em`, com identidade da classe
chamadora, para validar visibilidade. A API C de objetos sem essa validação é
interna/confiável e não é exposta como recurso da linguagem.

Descritores de tipos preservam identidades nominais de classes, vetores e tuplas
estruturais. Contratos de escrita instalados em vetores são mantidos através
de aliases e na clonagem; um alias genérico não enfraquece contratos anteriores.

## Exceções e limites

Erros de índice, tipo, aridade, conversão, overflow e deslocamento continuam
verificados com NDEBUG. Com um frame ativo, tornam-se exceções; sem tratador,
encerram o processo com status 1. `delegua_localizar` registra a localização da
fonte para o diagnóstico seguinte.

Frames de exceção ficam no heap gerenciado. Backends que usam setjmp devem
preservar corretamente locais modificados entre setjmp/longjmp e limpar os
frames em retornos e saídas de laços, como faz o backend C do CGD. O contador de
chamadas é restaurado no desenrolamento; a profundidade máxima é 1024.

Apenas falso e nulo são falsos. Divisão comum pode retornar real; divisão inteira
é explícita. Operações inteiras de 64 bits verificam overflow. Não há inteiro
arbitrariamente grande neste runtime. A aleatoriedade usa a biblioteca C e não
é criptográfica.

O runtime e seus estados globais não oferecem sincronização entre threads.
FFI exige funções com a ABI correta e código nativo confiável; handles fechados
são invalidados, mas não há isolamento de código externo.

## Unicode e reprodução

A fonte das tabelas está registrada no cabeçalho gerado. Para regenerar com a
versão Unicode fornecida pelo Python instalado:

```sh
python3 tools/gerar_unicode.py
```

Python não é necessário para compilar ou usar as tabelas já versionadas.
