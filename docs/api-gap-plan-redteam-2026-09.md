# Red team — alterações em mdio-cpp (`feat/api-gap-plan`)

**Data:** 2026-09-14 · **Objeto:** os 3 commits locais sobre `fcbfb85` (== `main`) —
`5d3f1dd` (plano de paridade), `4e6a61b` (correção do item de semântica de origem),
`9046367` (retirada do item tensorstore, refutado) — toda a divergência é o arquivo
novo `docs/api-gap-plan.md` (232 linhas).

**Método:** painel de 5 especialistas — arquitetura, code design, otimização,
flexibilidade, documentação — com modelos heterogêneos via aliases do bridge
(4 assentos de raciocínio máximo + 1 verificador), seguido de consolidação por
auditor factual, que verificou as citações decisivas linha a linha e rotulou cada
alegação (**fato confirmado** / **inferência** / **não-verificado**). As citações-chave
dos achados CRÍTICOS foram re-verificadas pelo orchestrator (dataset.h:655-695,
stats.h:229, variable.h:876-889, coordinate_selector.h:255-257,
dataset_test.cc:812-823, grep `to_json` = 0 em `mdio/*.h`).

**Postura:** adversarial — as premissas do autor do plano foram tratadas como
hipóteses a verificar. Nenhuma mutação foi aplicada ao plano; os achados abaixo
são a entrega.

---

## Relatório consolidado (auditor)

**Material e método.** Toda citação abaixo foi verificada por este auditor no
material apontado: base `mdio/` e `mdio/zarr/` (incl. `mdio/dataset_test.cc`),
plano `docs/api-gap-plan.md` ("plan"), `evaluation-report-2026-09.md` ("report").
Cada achado carrega rótulo: **fato** (citação verificada), **inferência**
(raciocínio declarado) ou **não-verificado**.

### 1. Veredito executivo

M1 e M2 têm premissas centrais confirmadas no código (nenhum `to_json` existe —
grep em `mdio/*.h` = 0 resultados; `get_chunk_shape` existe, variable.h:1433),
mas carregam omissões reais: header variables fora do round-trip e nenhum gate
de performance. **M3 e M4 exigem rework antes de qualquer PR**: M3 propõe API
que já existe (`Dataset::sel`, dataset.h:618-867) partindo de citação falsa
(`CoordinateSelector::query()` não existe; o erro está em `_applyOp`,
coordinate_selector.h:255-256); M4 colide com `internal::SummaryStats`
(stats.h:229-335) e `UpdateAttributes` (variable.h:881-889), inclui snippet que
não compila (`Histogram` abstrata, stats.h:82-107) e quebraria callers por name
hiding. O roadmap não contém milestone para o gargalo medido no relatório
(custo de metadados NFS, report:594-607).

### 2. Achados consolidados

#### CRÍTICO

**C1 — M3 propõe API que já existe; a premissa "sel by value unimplemented"
(plan:21) é falsa.**
Convergência: assentos 1, 2, 4, 5 (4/5). **Fato**: `Dataset::sel(Descriptors...)`
existe (dataset.h:618-867); seleção por valor funcional para Value
(dataset.h:704-734; testes dataset_test.cc:678/726/761) e Range (dataset.h:766-864;
teste :838). A List é bloqueada na própria validação de `sel`
(`UnimplementedError`, dataset.h:661-662, retorno antecipado :687-691; a condição
cobre qualquer `ListDescriptor<T>` — outer_type variable.h:154-161, membro `type`
:145-151), tornando o branch List (:735-765) código morto via `sel()`. Endpoints
de range exigem match exato ("Start value not found", dataset.h:819-821).
**Direção**: reformular M3 como *completar o `sel` existente* (List + endpoints
não exatos) e consolidar as camadas `Dataset::sel`/`CoordinateSelector`, não
adicionar segunda API.

**C2 — M3 se funda em citação inexistente e confunde camadas.**
Convergência: 1, 2, 5 (3/5) + 4 descreve a mesma camada (4/5). **Fato**: nenhum
método `query()` (grep "query" = só a string do erro, coordinate_selector.h:256);
o `UnimplementedError` está no `_applyOp` privado (coordinate_selector.h:239-258),
alcançável via ReadDataVariables (:88-101); `filterByCoordinate` por valor já
funciona (:113-120). O plano leu a mensagem de erro, não o código (plan:92).
**Direção**: reancorar M3 em `Dataset::sel` e corrigir plan:92.

**C3 — A assinatura proposta contradiz a semântica declarada.**
Convergência: 1, 2 (2/5). **Fato**: plan:99 diz "Selection by coordinate VALUE
(not index)" e plan:100-101 propõe
`std::variant<RangeDescriptor<Index>, ListDescriptor<Index>>`; os descritores
existentes são value-typed por documentação ("not supported for index-based
slicing", variable.h:122-123, 138-139) e o caminho sel→isel produz
`RangeDescriptor<Index>` como moeda do isel (dataset.h:717-722, 851-854). A
colisão das duas `sel` variádicas (overload morto/breaking, contra o princípio 3,
plan:229-230): **inferência** declarada, não simulada. **Direção**: descritores
no tipo da coordenada (o trait `extract_descriptor_Ttype` já existe,
variable.h:175-193) ou extensão do sel atual.

**C4 — M4 colide com `mdio::internal::SummaryStats` e reverte decisão documentada.**
Convergência: 1, 2, 5 (3/5); 4 no espírito (4/5). **Fato**: SummaryStats existe no
mesmo header proposto (plan:127; stats.h:229-335) com campos homônimos const
int32/float (stats.h:329-334), desserializados como int32/float (stats.h:272-275)
— o `double`/`Index` do plano (plan:128) diverge do statsV1; UserAttributes já
recebe coleções de stats (stats.h:547-549, 570-573); stats.h:69-72 declara por
design não haver caminho fácil para acrescentar histograma. **Direção**: reusar/
estender SummaryStats e publicar via UserAttributes/JSON, declarando a mudança
de política.

**C5 — M4: `UpdateAttributes(const SummaryStats&)` quebra a API existente e
inverte contrato.**
Convergência: colisão 1, 2, 4, 5 (4/5); mecanismo de name hiding 2 (1/5).
**Fato**: `VariableBase::UpdateAttributes(const nlohmann::json&)` retorna
`Result<void>` e não persiste (variable.h:847, 881-889; "does not commit changes
to durable media", :878-879; stats.h:37-39); `Variable : public VariableBase`
(variable.h:1016); declarar o mesmo nome na derivada (plan:133, com `absl::Status`
e persistência) esconde a sobrecarga da base (regra C++ de lookup) e cria irmãs
com contrato de erro e durabilidade opostos. **Direção**: outro nome e
`Result<void>`; persistir via CommitMetadata.

**C6 — M4: o snippet não compila.**
Convergência: 2 (1/5). **Fato**: `Histogram` é abstrata (métodos puramente
virtuais, stats.h:82-107) e a base a carrega por
`std::unique_ptr<const Histogram>` (stats.h:334); o plano declara
`Histogram histogram;` como membro (plan:128-129). **Direção**: ponteiro único
ou template no tipo de histograma.

**C7 — O roadmap não ataca o gargalo medido; os gates permitem fechá-lo sem
mover performance.**
Convergência: 3 (1/5); eco em 4. **Fato**: o plano cobre só os seis gaps
(plan:15-25, 211-220) com aceitação por "code shrinkage" (plan:5-10); o relatório
mede ≈12k syscalls de metadados por leitura de 16 chunks e 41× entre grids
equivalentes (report:594-607, 645-652), modelo aditivo (report:609-620),
56 min vs 0,75 s (report:628-632), e registra as correções: chaves de chunk
computadas do grid e cache de lookups (report:667-673). Que M2/M6 como escritos
preservam o caminho caro (leitura segue `tensorstore::Read(store)`,
variable.h:1078-1080; M6 "Not scheduled", plan:165-167): **inferência**
declarada, consistente com o código. **Direção**: milestone de metadata-path com
as direções do report + gate de performance (latência/região, syscalls) nos
milestones de I/O.

#### IMPORTANTE

**I1 — M1 omite header variables.** 1, 4 (2/5). **Fato**: Open separa
HeaderVariableCollection (dataset.h:969-983); o Dataset os carrega (dataset.h:172)
e serializa por caminho próprio (dataset.h:1331-1333); são "metadata-only…
TensorStore cannot open as arrays" (header_variable.h:100-106). M1 cobre só
`Variable::get_spec` (plan:45-49, 53-54) — a garantia plan:37-38 falha para
datasets com header variables. **Direção**: serializá-los explicitamente.

**I2 — M1 colapsa path/versão/contexto num JSON único.** 4 (1/5; rotulado
CRÍTICO pelo assento 4). **Fato**: `from_json(json_schema, path, zarr_version, …)`
(dataset.h:291-295); Dataset carrega `tensorstore::Context` (dataset.h:171/177);
backend resolvido por prefixo file/gcs/s3 (zarr_driver.h:223-227). A garantia
literal de plan:37-38 não fecha sem tratar binding. Separar "schema lógico" de
"binding de store": inferência de design. **Direção**: decidir a política de
binding no plano.

**I3 — Localização errada (plan:28).** 1, 5 (2/5). **Fato**: `from_json` está em
dataset.h (292/340/373); dataset_factory.h tem `from_json_to_spec` (:620). A
premissa "no inverse" é correta (grep `to_json` em mdio/*.h = 0). **Direção**:
corrigir o endereçamento.

**I4 — "Sequencing" esconde o fix 4.** 1, 5 (2/5). **Fato**: wave 0 = "Small
fixes 1–3" (plan:215); fix 4 (plan:187-196) fora de toda wave, embora M2 dependa
da decisão de origem (plan:79) e M3 seja apontado como sua correção
(plan:194-195). **Direção**: alocá-lo como pré-requisito de M2/M3.

**I5 — Small fix 3: decisão aberta + afirmação não sustentada pelo corpus.**
1, 2, 4, 5 (4/5). **Fato**: o "ou" indecidido (plan:182-186); schema exige
name/apiVersion/createdOn (dataset_schema.h:333-372, "required" :368-372); Open
rejeita modelo v0 (dataset.h:1026-1033). A contradição apontada pelo assento 5
**confirma-se com linhas corretas**: o report diz que o mdio-python 1.0.8
**escreve** `createdOn` (separador de espaço rejeitado pelo C++ — report:533-535,
764; as linhas 351/552 dadas pelo assento 5 são imprecisas). Matização: o reader
v3 converte `_ARRAY_DIMENSIONS`→`dimension_names` (zarr_v3.h:769-785).
**Direção**: política explícita de compatibilidade com evidência própria antes
de PR.

**I6 — M2 cita método inexistente.** 2, 5 (2/5). **Fato**: o acessor é
`dimensions()` (variable.h:1141); não há `Variable::domain()` (grep: só
`store.domain()`); `get_chunk_shape()` existe (variable.h:1433-1481).
**Direção**: corrigir plan:77.

**I7 — M2 é ergonomia; preserva o caminho de I/O medido.** 3 (1/5). **Fato**:
plan:75-86; variable.h:1078-1080; report:628-632 ("keep chunks large"). Que isso
normaliza o sintoma: inferência declarada. **Direção**: API bulk/fused com gate
de performance (ver C7).

**I8 — M2 expõe layout físico sem política declarada.** 4 (1/5). **Fato**:
`get_chunk_shape` normaliza 4 grafias de chave (variable.h:1453-1478);
`transform_chunks` cai para "single chunk per array" sem chunkGrid
(dataset_factory.h:528-547). **Direção**: explicitar política (físico/lógico,
travessia, evolução).

**I9 — M5 sem contrato async/dtype/rollback.** 2 e 4 (2/5 somados). **Fato**: a
base é async (Write→WriteFutures, variable.h:1127-1128; composição de futures em
dataset.h:986-1019); M5 propõe `absl::Status` síncrono com `ElementTransform`
indefinido (plan:155-158). **Direção**: definir callback, retorno Future e
reversibilidade antes de promover.

**I10 — Contagem "12 example programs" (plan:6-7) não bate com o relatório.**
5 (1/5). **Fato**: report:152 registra 10 programas no fim da Fase 0 e report:184
registra 22 no final; "12" não aparece no report. Origem como snapshot
intermediário: **não-verificado** (o doc não diz). **Direção**: corrigir ou datar.

#### SUGESTÃO

**S1** — M2: decidir header no plano, não na implementação (plan:67; variable.h
tem 1.997 linhas). 1 (1/5). Fato.
**S2** — Acrescentar critério "byte-idêntico" às aceitações por contagem
(plan:56/85/115/144), padrão do report:484-489. 1 (1/5). Fato.
**S3** — "no full coordinate reads" (plan:115-116) contradiz o design
(plan:106-108) e a base, que lê a coordenada inteira (var.Read(), dataset.h:549
e 790). 2, 5 (2/5). Fato.
**S4** — Números downstream (224→60, ~460, 985, 358→120, 246→80, 532→150, 628,
~5.5k/~2k) sem fonte auditável; o report só corrobora "~700" (report:216-217).
5 (1/5). Não-verificado — marcar como não auditado no plano.

### 3. Conflitos entre assentos (resolvidos com evidência)

1. **"sel implementa List com testes passando" (1) vs "List bloqueado" (2, 5).**
   O código bloqueia (dataset.h:661-662, retorno antecipado :687-691). O teste
   citado pelo assento 1 existe e **assertiona sucesso** (dataset_test.cc:812-823)
   — em tensão direta com a guarda; os testes de lista repetida/ausente esperam
   falha (:825-836, :875-897). **Novo achado do cruzamento**: teste e guarda
   mutuamente excludentes na base; como não é possível executar a suíte aqui,
   qual dos dois está quebrado neste commit é **desconhecido**. A leitura do
   assento 1 ("testes passando" incluindo selList) não se sustenta contra o
   código; vale a síntese 2/5: Value/Range funcionam, List está bloqueado na
   validação.
2. **`dimensions()` em 1141 vs 1213.** Resolvido: declaração em variable.h:1141;
   1213 é chamada dentro de `sliceInRange`. Substância (não existe `domain()`)
   confirmada por grep — assento 2 correto na linha, assento 5 correto na
   substância.
3. **"M1/M2 sólidos" (2) vs ressalvas (1, 3, 4).** Compatíveis: premissas centrais
   confirmadas; as omissões (header variables, binding, performance) são fatos
   verificados. Rework para M3/M4; ajustes de escopo em M1/M2.

### 4. Pontos fortes reconhecidos

Small fix 1 é exato (erro imprime o descritor original enquanto o check falha no
clampado — variable.h:1306, 1317-1321, 1330-1335; confirmado por 2 assentos e
por este auditor). Small fix 2 confirmado (fallback silencioso para v2,
zarr_driver.h:120-122; mensagem em zarr_v2.h:363). Item 5 retirado é exemplar
(plan:197-209; consistente com report:152-166); M6 corretamente não-agendado
(plan:165-167); derivação de gaps a partir de código consumidor é método sólido
(assento 1). Premissa do M1 correta (grep `to_json` = 0) e a citação
`builder/schemas/v1/stats.py` confere no clone (assento 5; não re-verificado por
este auditor).

### 5. Itens não-verificáveis no material disponível

- Execução da suíte de testes — incl. se `selList` passa hoje (teste e guarda em
  tensão, §3.1).
- Hash do fork tensorstore `917edaf34` e o "0-based GetChunkGridBounds"
  @457285c (plan:189-190).
- Números downstream (S4) e a origem da contagem "12" (I10).
- Comportamento externo zarr-python #2134 (documentado em report:450-509; não
  reproduzível aqui).
- mdio-python (`api/io.py`, `builder/schemas/v1/stats.py`): verificado pelo
  assento 5; fora do escopo deste auditor.
- Estado git do branch: estipulado pelo cenário, corroborado por report:211-213.

---

## Assento 1 — Arquitetura (painel-raciocinio-1, glm-5.3 max via raciocinio-max-1)

### CRÍTICO

- **M3 diagnostica errado o gap e propõe uma API que colide com `Dataset::sel`
  existente.** O plano afirma que "`CoordinateSelector::query()` returns
  `UnimplementedError` for `RangeDescriptor`/`ListDescriptor`" (plan:92) — não
  existe método `query()`; o erro está em `_applyOp` (coordinate_selector.h:255-256),
  alcançável só via `ReadDataVariables` (coordinate_selector.h:88-101). Enquanto
  isso, `Dataset::sel` **já implementa seleção por valor** para Value/List/Range
  (dataset.h:618-867, ex. List em :735-765), com testes passando
  (dataset_test.cc:678-919, `selList`:812, `selRange`:838). A assinatura proposta
  `Result<Dataset> Dataset::sel(std::variant<RangeDescriptor<Index>,
  ListDescriptor<Index>>... descs)` (plan:100-102) (a) é Index-tipada,
  contradizendo o próprio objetivo "Selection by coordinate VALUE (not index)"
  (plan:99); (b) cria segunda `sel` variádica na mesma classe — o template
  existente (dataset.h:618) vence a resolução na maioria das chamadas (overload
  morto) ou substitui silenciosamente a API (breaking, exigindo nota de
  deprecação, plan:229-230). O plano ignora ainda que existem **duas camadas de
  seleção paralelas** (`Dataset::sel` vs `CoordinateSelector`) com
  responsabilidades sobrepostas — a questão arquitetural real.
- **M1 funda-se em `Variable::get_spec()`, sabidamente quebrado para struct
  arrays, e o fix não está agendado em lugar nenhum.** O relatório que motivou o
  plano documenta: o spec derivado de variável estruturada vem como `"byte"` e é
  **rejeitado pelo próprio schema de criação do mdio-cpp**
  (evaluation-report:519-523), com perda dentro da API da biblioteca
  (`Variable::get_spec()`, variable.h:1406; evaluation-report:765), e
  `CommitMetadata` apaga o dtype por não ser round-trippable
  (dataset.h:1293-1295). M1 diz "Build on `Variable::get_spec()`" (plan:45) e
  promete round-trip com "struct arrays" nos testes (plan:53-54). Sem o fix da
  derivação (Issue 02) e do dialeto de escrita `"struct"` vs `"structured"`
  (Issue 01, zarr-python #2134, evaluation-report:450-509), M1 não cumpre a
  própria aceitação — e nem a condição de entrada do Issue 02 (`createdOn`,
  evaluation-report:531-536) está no plano. Pré-requisito implícito ausente do
  "Sequencing".
- **M4 colide com `SummaryStats` existente e inverte um contrato documentado.**
  `mdio::internal::SummaryStats` já existe (stats.h:229-335): imutável, campos
  `const float`/`const int32_t count` (stats.h:329-334), desserializado como
  float/int32 (stats.h:272-275). O plano propõe `struct SummaryStats { Index
  count; double sum... }` no mesmo header (plan:128-131) sem dizer se reutiliza,
  estende ou substitui — e `double` truncará ao persistir pelo caminho statsV1
  existente. Além disso, `Variable::UpdateAttributes` já existe com outra
  assinatura/retorno (`Result<void>` para JSON, variable.h:882) vs. `absl::Status`
  proposto (plan:133) — convenções de erro divergentes na mesma classe. E
  stats.h:69-72 declara por design que **não** se adiciona histograma a
  UserAttributes existente — M4 reverte essa decisão sem admiti-lo.

### IMPORTANTE

- **Round-trip do M1 omite header variables.** `Dataset` carrega
  `header_variables` (dataset.h:172, :969-983) e `CommitMetadata` os serializa
  via `ToCommitJson()` (dataset.h:1331-1333); as design notes do M1 (plan:45-49)
  e os testes (plan:53-54) só cobrem `Variable::get_spec()`. A garantia
  "from_json(to_json(ds)) opens an equivalent dataset" (plan:37-38) falha
  silenciosamente para datasets com header variables.
- **Sequencing esconde pré-requisito: o fix 4 (semântica de origem) não está em
  nenhuma wave.** Wave 0 = "Small fixes 1–3" (plan:215); o fix 4 (plan:187-196)
  fica fora, mas M2 o cita como base ("domains are 0-based today", plan:79) e o
  próprio fix 4 o contradiz (slicing gera domínio `[83,383)`, plan:192-193). A
  decisão origem-absoluta-vs-relativa dos boxes do `ChunkRange` e o mapeamento
  valor→índice em domínios com offset são pré-requisitos de M2 e M3 — a tabela
  não os captura.
- **Fix 3 (wave 0) carrega decisão arquitetural aberta.** "Either tolerate
  missing metadata with defaults in C++, or contribute the metadata writing to
  mdio-python" (plan:183-186) — leitor leniente vs. escritor estrito são direções
  opostas, com efeito no que `to_json` emite para campos defaultados e no
  detector de modelo v0 (dataset.h:1026-1034). Item de wave 0 não deveria ter um
  "ou" indecidido.
- **M3 adia semântica que a mdio-python já define.** "descending coords (reject
  or support — decide)" (plan:113) — o princípio 1 é paridade com mdio-python
  (plan:224); adiar a decisão para a hora do teste cria risco de interop. O mesmo
  vale para "values outside the range (error, not clamp)" (plan:112-113),
  afirmado sem verificação contra o comportamento Python.
- **M3 duplicaria uma terceira cópia da lógica valor→índice.** "binary-search
  value → index, delegate to isel" (plan:106-108) repete o que
  `descriptor_to_index` já faz por varredura linear (dataset.h:531-608). O plano
  deveria consolidar as duas camadas existentes, não somar implementação.

### SUGESTÃO

- **Citação imprecisa:** "`Dataset::from_json()` (dataset_factory.h)" (plan:28) —
  `from_json` está em dataset.h:292-382; dataset_factory.h tem `Construct`. Em
  plano cujos itens declaram "where it lands", o endereçamento deve estar certo.
- **M2: decidir o header no plano, não na implementação** ("variable.h (or
  mdio/chunk_iterator.h)", plan:67) — variable.h já tem ~2000 linhas; header novo
  é a escolha coerente com as fronteiras. Testes do M2 deveriam incluir domínios
  com offset (pós-`isel`), dado o fix 4.
- **Critérios de aceitação só por contagem de linhas** (plan:56, 85, 115, 144) —
  acrescentar "saída byte-idêntica à versão workaround"; o relatório já usa esse
  padrão (chunks bit-idênticos, evaluation-report:484-489).

**Pontos fortes:** o item retirado #5 é exemplar (causa raiz com UB documentado,
plan:197-209); M6 corretamente não-agendado; a derivação de gaps a partir de
código consumidor é método sólido — o problema é que o plano não cruzou seus
próprios achados (Issues 01/02 do relatório) com as fundações dos milestones.

---

## Assento 2 — Code design (painel-raciocinio-2, glm-5.3 max via raciocinio-max-2)

### CRÍTICO

- **M3 propõe adicionar uma API que já existe na base.** `Dataset::sel` já está
  implementado: `template <typename... Descriptors> Result<Dataset>
  sel(Descriptors...)` (dataset.h:618-867) — ValueDescriptor funcional
  (dataset.h:705-734; testes selValue/selRepeatedValue/selMultipleValues,
  dataset_test.cc:678/726/761) e RangeDescriptor por valor funcional
  (dataset.h:766-864; teste dataset_test.cc:838). O gap real é apenas:
  ListDescriptor bloqueado na validação (dataset.h:661-662, `UnimplementedError`)
  e endpoints com match exato obrigatório ("Start value not found",
  dataset.h:819-821 — o `sel(inline=slice(a,b))` do mdio-python não exige valores
  exatos). A premissa do plano ("`sel` by value unimplemented", api-gap-plan.md:21)
  é falsa; a proposta duplica superfície sem citar o método existente. Correção:
  reformular M3 como *completar* `sel` (List + endpoints não-exatos), não
  *adicionar*.
- **M3: a assinatura contradiz a semântica declarada.** O plano diz "Selection by
  coordinate VALUE (not index)" (api-gap-plan.md:99) mas propõe
  `std::variant<RangeDescriptor<Index>, ListDescriptor<Index>>`
  (api-gap-plan.md:100-101) — descritores `<Index>` só expressam índices (isso é
  `isel`); valor exige `RangeDescriptor<T>` com `T` = tipo da coordenada. O trait
  para isso já existe (`extract_descriptor_Ttype`, variable.h:176-193). Além
  disso, `std::variant` em pack desvia do idioma da base (pack heterogêneo de
  descritores: dataset.h:618, variable.h:1302-1303).
- **M4: `Variable::UpdateAttributes(const SummaryStats&)` quebra compilação
  existente por name hiding.** A base tem `VariableBase::UpdateAttributes(const
  nlohmann::json&)` (variable.h:881-889); declarar o mesmo nome na derivada
  `Variable` esconde a sobrecarga herdada — todo caller `var.UpdateAttributes(json)`
  para de compilar sem `using VariableBase::UpdateAttributes;`. Viola o Princípio
  3 do próprio plano ("No breaking changes", api-gap-plan.md:229-230).
- **M4: o snippet não compila contra a base.** `Histogram histogram;` como membro
  (api-gap-plan.md:128-129) é ill-formed — `Histogram` é classe abstrata
  (stats.h:82-107, métodos puramente virtuais); a própria base usa
  `std::unique_ptr<const Histogram>` (stats.h:334).

### IMPORTANTE

- **M4: mesmo nome `UpdateAttributes`, contrato de persistência oposto.** A base
  documenta "This does not commit changes to durable media. See the Dataset
  CommitMetadata method" (variable.h:876-879; stats.h:37-39); o plano diz
  "Persists as statsV1 user attributes" (api-gap-plan.md:132-133). Sobrecargas
  irmãs com durabilidade diferente são armadilha. E retorna `absl::Status` onde a
  base retorna `Result<void>` (variable.h:882) — contrato de erro inconsistente.
- **M4: colisão de nome com tipo existente.** `mdio::internal::SummaryStats` já
  existe (stats.h:229-335) na mesma header proposta (api-gap-plan.md:127), com
  tipos distintos (`int32_t count`, `float min/max/sum` — stats.h:329-333 vs
  `Index count; double...` no plano). `Index` para `count` diverge do statsV1
  (int32). E o requisito "incremental, chunk-composable, distributed callers can
  reduce partials" (api-gap-plan.md:130, 137-138) não é expressável na assinatura
  `ComputeStats(const Variable<>&)` — falta uma operação de merge de parciais na
  API proposta.
- **M3 cita método inexistente.** "`CoordinateSelector::query()`"
  (api-gap-plan.md:92) não existe; o `UnimplementedError` está no `_applyOp`
  privado (coordinate_selector.h:255-257 — a *string de erro* diz "query():", o
  método é outro; o plano leu a mensagem, não o código). E `CoordinateSelector`
  é caminho distinto de `sel` (filterByCoordinate por valor já funciona,
  coordinate_selector.h:113-120) — o plano os confunde.
- **M2: proposta não verificada contra nomes da base.** "Derive the grid from
  `domain()`" (api-gap-plan.md:77) — `Variable` não tem `domain()`; o acessor é
  `dimensions()` (variable.h:1141). `get_chunk_shape()` existe (variable.h:1433).
  A ideia do `ChunkRange` é boa e não colide, mas a nota revela checagem
  incompleta.
- **M3: critério de aceitação auto-contraditório.** "no full coordinate reads"
  (api-gap-plan.md:115-116) vs. nota de design "read the 1-D coordinate variable"
  (api-gap-plan.md:107) e a implementação existente, que faz `var.Read()` da
  coordenada inteira (dataset.h:549, 790). Ou o critério muda, ou o design
  precisa de busca sem materializar a coordenada toda.

### SUGESTÃO

- **M5: contrato assíncrono ausente.** `TransformVariable` retorna `absl::Status`
  síncrono (api-gap-plan.md:157) — desvia do idioma async da base (Write →
  WriteFutures, variable.h:1128; Open → Future). `ElementTransform` sem definição
  e o snippet sem local de arquivo (api-gap-plan.md:155-159).
- **Interação M1 ↔ small-fix-3 não conectada.** Tolerar metadados ausentes com
  defaults no leitor v3 (api-gap-plan.md:185-186) exige rever a validação
  required do schema (`apiVersion`/`createdOn`, dataset_schema.h:342-371;
  dataset.h:1027) e fazer `to_json` sintetizar esses campos no round-trip — o
  plano trata os três isoladamente.
- **Small fix 1 verificado correto** — bom achado: o erro imprime
  `err.start/err.stop` do descritor original (variable.h:1306, 1330-1335)
  enquanto o check falha no clampado (variable.h:1318-1319).

**Síntese:** M1 e M2 são propostas sólidas e consistentes com a base. M3 e M4
foram escritos sem cruzar dataset.h/stats.h/variable.h — um propõe re-adicionar
API existente com assinatura semanticamente errada, o outro propõe código que não
compila e quebra callers. Recomendo rework de M3/M4 antes de qualquer PR.

---

## Assento 3 — Otimização (painel-raciocinio-3, gpt-5-4-petrobras xhigh via raciocinio-max-3)

- **CRÍTICO**: o roadmap não ataca a causa medida do colapso de performance. O
  documento se assume como plano de "API gaps" e só enumera seis lacunas de API
  (`to_json`, iteração de chunks, `sel`, stats, transferência type-erased,
  execution layer) e suas waves de entrega, sem nenhum milestone para custo de
  metadados, endereçamento direto de chunk key ou cache de lookups
  (api-gap-plan.md:15-25, 211-220). Já a avaliação isolou precisamente isso como
  mecanismo: leitura de 16 chunks com **~12k syscalls de metadados**,
  `openat`/`getdents64`, custo proporcional à contagem total de chunk files,
  **41×** entre grids equivalentes, e fix directions explícitas: "construct chunk
  keys directly from the grid" e "cache negative/positive metadata lookups"
  (evaluation-report:594-607, 645-673). **Fix**: inserir um milestone de
  otimização do metadata path antes/ao lado de M2.
- **CRÍTICO**: os critérios de aceitação do plano permitem "fechar o roadmap" sem
  mover a performance. O documento diz que cada item terá critério "expressed as
  downstream code shrinkage" (api-gap-plan.md:5-10) e os milestones seguem isso
  (api-gap-plan.md:56-57, 85-86, 115-116, 144-145, 227-228). Enquanto isso, a
  avaliação já tem um modelo de custo aditivo explícito, `0.07 s + 0.0025 s ×
  source files + 0.0026 s × accumulated destination files`
  (evaluation-report:609-620). **Fix**: qualquer milestone que toque
  leitura/escrita deve ganhar gate de performance: latência por região, contagem
  de syscalls de metadados e/ou perda da dependência linear com source chunk
  files.
- **IMPORTANTE**: M2 ("iterar chunks") é, como está escrito, ergonomia de API;
  não é otimização de I/O. O design só propõe "Derive the grid from `domain()` +
  `get_chunk_shape()`" e devolver `Box<>` por chunk (api-gap-plan.md:75-80). Mas
  a base já expõe `get_chunk_shape()` (variable.h:1419-1480), já faz `slice` por
  intervalo (variable.h:1348-1353) e a leitura continua sendo
  `tensorstore::Read(store)` (variable.h:1078-1083). Ou seja: M2 remove o triple
  loop manual, mas preserva intacto o caminho caro que a avaliação mediu
  (evaluation-report:594-607). **Sugestão concreta**: M2 só faz sentido
  acompanhado de uma API bulk/fused (`read_boxes`, `map_regions`, ou equivalente)
  com reuso de estado entre boxes.
- **IMPORTANTE**: M2 ainda corre o risco de institucionalizar a granularidade
  errada. O acceptance dele é "downstream trace reader ≤120 lines ... with no
  manual chunk loops" (api-gap-plan.md:85-86). Isso empurra consumidores para
  "um box por chunk" como padrão. Só que a avaliação concluiu exatamente o
  oposto do ponto de vista de custo: até o fix, "keep chunks large" porque muitos
  trabalhos pequenos explodem o overhead; o caso distribuído foi **56 min** em
  383 tasks vs **0.75 s** em uma task (evaluation-report:628-632). Sem uma
  contramedida explícita, M2 normaliza o sintoma.
- **IMPORTANTE**: M6 ("execution layer") está no nível errado de abstração para o
  gargalo medido. O texto só fala em `MapChunks(variable, fn, ParallelOptions)`
  sobre futures do TensorStore e ainda "Not scheduled" (api-gap-plan.md:163-167).
  Mas a base já tem composição assíncrona/futures para abrir variáveis e esperar
  conjuntos delas (dataset.h:985-1019, variable.h:495-549). E a avaliação
  descartou contenção/serialização como causa: com `K=8`, o custo não piora; o
  problema é o trabalho por task, não falta de paralelismo
  (evaluation-report:622-624). **Fix**: se M6 existir, ele precisa especificar
  fusão/batching de regiões, reuso de open handles e/ou cache de metadados;
  "scheduler sobre chunks" sozinho não resolve.
- **IMPORTANTE**: a seção "Sequencing" é arriscada sob a ótica de custo. Ela põe
  `to_json`, iteração de chunks, `sel` e stats antes de qualquer trabalho de
  transferência/execução (api-gap-plan.md:211-220). Para ergonomia, isso é
  plausível; para performance, prioriza conveniência enquanto deixa intacto o
  maior custo operacional já medido (evaluation-report:628-632). Eu inverteria:
  primeiro metadata-path optimization; depois M2/M6 redesenhados em cima desse
  custo model.
- **SUGESTÃO**: adicionar um milestone explícito de performance com entregáveis
  nomeados pela própria avaliação: 1) endereçamento direto de chunk keys a partir
  do grid armazenado; 2) cache de lookups positivos/negativos dentro do processo;
  3) API bulk para múltiplas regiões por open. Essas são exatamente as "fix
  directions" registradas (evaluation-report:667-673); o texto atual de M2/M6 não
  as cobre (api-gap-plan.md:67-72, 165-167).
- **SUGESTÃO**: escrever a limitação de backend no próprio roadmap. O material só
  mede NFS e diz explicitamente que "other backends (S3, GCS, local) may not
  exhibit it" (evaluation-report:658-659). Então, se a intenção é cobrir object
  store também, falta um modelo de custo/backend matrix; se não, o plano deve
  declarar que esta priorização de performance é NFS-específica.

---

## Assento 4 — Flexibilidade (painel-raciocinio-4, gpt-5-4-petrobras xhigh via raciocinio-max-4)

Leitura estratégica: o plano está mais forte como catálogo de dores do consumidor
do que como roadmap de uma biblioteca extensível. Os maiores riscos de
congelamento prematuro estão em M1 e M3.

- **CRÍTICO**: M1 confunde snapshot lógico com receita de recriação. O plano
  promete `to_json()` como inverso de `from_json()` e fala em
  "`from_json(to_json(ds))` opens an equivalent dataset" (docs/api-gap-plan.md:36-49),
  mas `Dataset::from_json` hoje sempre recebe `path` e opcionalmente
  `zarr_version` (mdio/dataset.h:268-349), enquanto o `Dataset` também carrega
  `tensorstore::Context` (mdio/dataset.h:166-178,1416-1419) e o backend é
  resolvido separadamente para `file`/`gcs`/`s3` (mdio/zarr/zarr_driver.h:220-229,
  302-333). Isso colapsa três políticas distintas — store location, versão de
  formato e contexto/credenciais — num JSON único. **Melhor**: separar "schema
  lógico" de "binding de store" (path/backend/version/context), em vez de fazer
  de `to_json()` o canônico de tudo.
- **CRÍTICO**: M3 mira o lugar errado e arrisca mexer na API pública já
  existente. O plano apresenta "M3 — Value-based selection: `Dataset::sel()`"
  como se fosse API a adicionar (docs/api-gap-plan.md:90-102), mas
  `Dataset::sel(Descriptors...)` já existe (mdio/dataset.h:618-866). O
  `UnimplementedError` citado está na camada `CoordinateSelector` para Range/List
  (mdio/coordinate_selector.h:240-257), não no `Dataset`. Se o milestone
  introduzir outro `sel()` ou redefinir o atual sem matriz explícita de
  compatibilidade, o plano congela cedo uma semântica que hoje já é pública.
  **Melhor**: tratar isso como extensão/correção do `sel` existente ou da
  `CoordinateSelector`, não como API nova.
- **IMPORTANTE**: M1 ignora uma extensão que a base já tem: `HeaderVariable`. O
  desenho diz "Build on `Variable::get_spec()`" (docs/api-gap-plan.md:45-48), mas
  `Dataset::Open` já separa `HeaderVariable`s (mdio/dataset.h:969-982), o
  `Dataset` os expõe como coleção própria (mdio/dataset.h:1406-1407), e eles
  existem justamente para dtypes metadata-only que o TensorStore não abre como
  array (mdio/header_variable.h:100-106). Um `to_json()` centrado só em
  `Variable` estreita a biblioteca para datasets "tensorstore-openable" e perde
  flexibilidade já presente. **Melhor**: declarar no plano como serializar
  `header_variables` e outros casos fora de `Variable::get_spec()`.
- **IMPORTANTE**: M4 especializa cedo demais algo que a base tratou como
  mecanismo genérico de metadados. O plano propõe
  `Variable::UpdateAttributes(const SummaryStats&)` (docs/api-gap-plan.md:127-134).
  Hoje a API pública já tem `UpdateAttributes(const nlohmann::json&)`
  (mdio/variable.h:881-889), e `UserAttributes` é descrito como "extensible data
  class" (mdio/stats.h:32-45). Fazer `statsV1` virar método dedicado endurece a
  API em torno da versão atual do atributo e aumenta o custo de evoluir para
  outros metadados/versionamentos. **Melhor**: manter `ComputeStats(...)`
  separado e publicar o resultado pelo mecanismo genérico de
  `UserAttributes`/JSON.
- **IMPORTANTE**: M2 transforma layout físico de chunk em contrato público sem
  declarar política. O plano quer `Variable::chunks()` sobre "the variable's
  chunk grid" (docs/api-gap-plan.md:61-80). Só que a base trata chunking como
  detalhe de formato: `ZarrLayout` centraliza diferenças v2/v3
  (mdio/dataset_factory.h:58-67,69-158), e `transform_chunks` cai para "single
  chunk per array" quando `chunkGrid` não existe (mdio/dataset_factory.h:529-547).
  `get_chunk_shape()` ainda normaliza várias chaves/layouts
  (mdio/variable.h:1433-1480). Se isso virar API central sem policy hook, você
  amarra o contrato ao layout físico atual. **Melhor**: explicitar se a API expõe
  chunk físico, tile lógico, ordem de travessia e como isso evolui com
  rechunk/reformat.
- **IMPORTANTE**: o small fix de interop sobre metadados ausentes está perigoso
  porque pode virar dialeto implícito. O plano diz: "Either tolerate missing
  metadata with defaults in C++, or contribute the metadata writing to
  mdio-python." (docs/api-gap-plan.md:182-186). Mas o schema exige `name`,
  `apiVersion`, `createdOn` (mdio/dataset_schema.h:333-372), e o `Open` atual já
  rejeita metadados de modelo antigo (mdio/dataset.h:1026-1033). Se a solução for
  "default silencioso", a biblioteca passa a aceitar um formato local não
  declarado. **Melhor**: isso precisa virar política explícita de compatibilidade
  estrita vs. leniente, não "small fix" solto.
- **IMPORTANTE**: a aceitação está superajustada ao consumidor atual e
  subespecifica portabilidade. O plano abre dizendo que cada item terá aceitação
  por "downstream code shrinkage" (docs/api-gap-plan.md:5-10), e os testes dos
  marcos cobrem v2/v3/struct ou casos locais (docs/api-gap-plan.md:53-54, 82-83,
  112-113, 141-142). Mas a base já abstrai `file`, `gcs`, `s3`
  (mdio/zarr/zarr_driver.h:220-229, 302-333) e usa `tensorstore::Context` na
  abertura (mdio/dataset.h:932-937, 1416-1419). Isso deixa passar uma API "boa
  para este consumidor em file/local" mas fraca como biblioteca portável.
  **Melhor**: adicionar uma matriz mínima backend × versão × dtype nas gates de
  cada marco.
- **SUGESTÃO**: antes de M5, faltam duas explicitações: contrato para dtype
  estruturado e custo de reversão. O esboço `TransformVariable(...,
  ElementTransform)` não diz se o callback enxerga escalar, campo selecionado,
  registro inteiro ou view `void` (docs/api-gap-plan.md:149-161), enquanto a base
  já tem caminhos versionados para struct (mdio/zarr/zarr.h:243-266;
  mdio/variable.h:442-460, 500-516; mdio/dataset.h:1110-1240). E as ondas
  registram ordem/tamanho, mas não reversibilidade (docs/api-gap-plan.md:59, 88,
  118, 147, 211-230). Eu exigiria isso antes de promover M5/M6.

---

## Assento 5 — Documentação (painel-verificador-1, glm-5.3-flash high via raciocinio-alto-1)

### CRÍTICO

- **M3/Gap 3 — "sel by value unimplemented" é falso.** `Dataset::sel` **já
  existe** e já implementa seleção por valor: `ValueDescriptor`
  (dataset.h:704-734) e `RangeDescriptor` com valores→índices→`isel`
  (dataset.h:766-864). Só `ListDescriptor` retorna `UnimplementedError` — em
  dataset.h:661-662, não onde o plano aponta. A evidência citada do plano
  (`CoordinateSelector::query()` → UnimplementedError, coordinate_selector.h:255-256
  — confirmada) é real mas **irrelevante para `Dataset::sel`**: o método
  existente não chama `query()`; tem conversão própria (`descriptor_to_index`,
  dataset.h:532-608, com varredura linear — var.Read() em 549/790). A API
  proposta `Dataset::sel(variant<RangeDescriptor,ListDescriptor>...)` colidiria
  com o template existente. Também derruba o critério de aceite "no full
  coordinate reads" (linha 115-116): o `sel` atual já lê a coordenada inteira
  internamente. M3 real = "implementar List + busca binária no sel existente",
  não "adicionar sel".

### IMPORTANTE

- **M1 citação errada:** "The library has `Dataset::from_json()`
  (dataset_factory.h)" (linha 28). `Dataset::from_json` é declarado em
  **dataset.h**:292/340/373; dataset_factory.h tem `from_json_to_spec` (linha
  620) e maquinaria de construção. A premissa "no inverse" está correta (nenhum
  `to_json` de Dataset em mdio/).
- **M4 ignora código existente com o mesmo nome:** `mdio::internal::SummaryStats`
  (stats.h:229-247) já define exatamente os campos propostos (count, sum,
  sumSquares, min, max, histogram), e `UserAttributes` já tem construtores que
  recebem coleções de stats (stats.h:547, 570). O delta real de M4 é só
  `ComputeStats` (nada computa — confirmado); a struct proposta duplica/colide. A
  citation `builder/schemas/v1/stats.py` (linha 139) está correta (arquivo existe
  no clone mdio-python).
- **Small fix 3 contradito pelo próprio corpus:** a afirmação "mdio-python
  `to_mdio` writes neither the root dataset metadata (name/apiVersion/createdOn)"
  (linhas 182-186) choca com a issue 03 do relatório — stores py **contêm**
  `createdOn` (é o C++ que o rejeita pelo separador de espaço; report linhas 351,
  552). `to_mdio` (mdio-python api/io.py:69-107) repassa os attrs do Dataset; não
  carimba nada. E a exigência de `dimension_names` do lado C++ é matizada pelo
  próprio reader v3, que converte `_ARRAY_DIMENSIONS`→`dimension_names`
  (zarr_v3.h:764-784). Item necessita evidência própria antes de virar PR.
- **Contagem "12 example programs" (linhas 6-7):** não aparece no relatório que
  motivou o plano — o relatório registra 10 programas no fim da Fase 0 (linha
  152) e 22 distintos no final (linha 184). Ou é snapshot de meio de avaliação (o
  doc não diz) ou está errado. Os "~5.5k/~2k linhas" são inverificáveis.

### SUGESTÃO

- **M2:** "Derive the grid from `domain()`" (linha 77) — `Variable` não tem
  método `domain()`; o acessor é `dimensions()` (variable.h:1213).
  `get_chunk_shape()` existe ✓ (variable.h:1433).
- **M4:** `UpdateAttributes(const SummaryStats&)` proposto como `absl::Status`
  sob comentário "// mdio/stats.h" (linhas 132-133); o `UpdateAttributes`
  existente (variable.h:882) retorna `Result<void>` e é método de Variable.
- **Sequencing:** wave 0 cobre "Small fixes 1–3" (linha 215), mas o item 4
  (domain origin semantics) não está em nenhuma wave.
- **Números downstream inverificáveis** (224→60, ~460, 985, 358→120, 246→80,
  532→150, 628): o relatório só corrobora o "~700 linhas" (linha 217). Citar
  caminho ou marcar como não auditado.

### Confirmado contra a base (sem achado)

Small fix 1 **exato**: check no descritor clamped (variable.h:1318-1321), erro
imprime o original (variable.h:1331-1335) — o exemplo "1004 > 1304 falso" é
mecanicamente consistente. Small fix 2 **exato**: `DetectVersion` faz fallback
silencioso para v2 (zarr_driver.h:121-122) e a mensagem citada existe
(zarr_v2.h:363). `get_spec()` ✓ (variable.h:1406); `CenteredBinHistogram`
counts/binCenters ✓ (dataset_schema.h:166-198); scrub de statsV1 ✓
(variable.h:369-370); base `fcbfb85` = v0.2.0-pre-release ✓ (report linha 81);
`b5e42fc` clamp ✓ (report linha 146); item 5 retirado consistente com report
linhas 153-156, 162-166 — mas o hash tensorstore `917edaf34` e o "0-based
GetChunkGridBounds" do fork @457285c **não são verificáveis** no material
fornecido.
