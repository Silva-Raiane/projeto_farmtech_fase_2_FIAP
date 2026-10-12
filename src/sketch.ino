/*
 * ============================================================================
 * FarmTech Solutions - Fase 2
 * Sistema de Irrigacao Inteligente com ESP32
 * ----------------------------------------------------------------------------
 * Cultura alvo: CAFE (Coffea arabica)
 *
 * Sensores simulados no Wokwi:
 *   - Botao verde  -> presenca de Nitrogenio (N)   [true/false]
 *   - Botao verde  -> presenca de Fosforo    (P)   [true/false]
 *   - Botao verde  -> presenca de Potassio   (K)   [true/false]
 *   - LDR          -> pH do solo (0 a 14, convertido do valor analogico)
 *   - DHT22        -> umidade do solo (%)
 *   - Rele azul    -> bomba d'agua da irrigacao
 *
 * OBS. IMPORTANTE SOBRE OS BOTOES
 * Os botoes do Wokwi sao momentaneos (so ficam pressionados enquanto o mouse
 * esta clicado), o que impediria manter N, P e K ativos ao mesmo tempo. Por
 * isso cada botao funciona aqui como LIGA/DESLIGA: um clique alterna o estado
 * do nutriente, que fica retido ate o proximo clique. Isso tambem representa
 * melhor a realidade, ja que o nivel de nutriente no solo e um estado continuo
 * e nao um pulso.
 *
 * Autora: Raiane Sirlene da Silva - RM 575984
 * ============================================================================
 */

#include <DHT.h>

// ---------------------------------------------------------------------------
// MAPEAMENTO DE PINOS
// ---------------------------------------------------------------------------
#define PINO_DHT        15   // DHT22 - umidade do solo
#define TIPO_DHT        DHT22
#define PINO_LDR        34   // LDR (ADC1_CH6) - pH do solo
#define PINO_BTN_N      18   // Botao verde - Nitrogenio
#define PINO_BTN_P      19   // Botao verde - Fosforo
#define PINO_BTN_K      21   // Botao verde - Potassio
#define PINO_RELE       23   // Rele azul - bomba d'agua

// ---------------------------------------------------------------------------
// PARAMETROS AGRONOMICOS DO CAFE
// ---------------------------------------------------------------------------
// O cafeeiro prefere solo levemente acido. Fora dessa faixa os nutrientes
// ficam indisponiveis para a planta e irrigar vira desperdicio de agua.
const float PH_MINIMO           = 5.0;
const float PH_MAXIMO           = 6.8;

// Faixas de umidade do solo (histerese para evitar o rele "piscando")
const float UMIDADE_CRITICA     = 35.0;  // abaixo disso: risco de estresse hidrico
const float UMIDADE_PREVENTIVA  = 50.0;  // zona de manutencao
const float UMIDADE_MAXIMA      = 60.0;  // acima disso: solo saturado, desliga

// Resolucao do ADC do ESP32 (12 bits)
const int   ADC_MAXIMO          = 4095;
const float PH_ESCALA_MAXIMA    = 14.0;

// Tempos
const unsigned long INTERVALO_LEITURA = 2000;  // ms entre leituras do DHT22
const unsigned long DEBOUNCE_MS       = 50;    // anti-repique dos botoes

// ---------------------------------------------------------------------------
// ESTRUTURA DOS BOTOES DE NUTRIENTE (modo liga/desliga)
// ---------------------------------------------------------------------------
struct Nutriente {
  const char*   rotulo;
  int           pino;
  bool          presente;          // estado retido: nutriente no solo
  bool          pressionadoAntes;  // ultima leitura estavel do botao
  unsigned long ultimaMudanca;     // controle de debounce
};

Nutriente nitrogenio = { "Nitrogenio", PINO_BTN_N, false, false, 0 };
Nutriente fosforo    = { "Fosforo",    PINO_BTN_P, false, false, 0 };
Nutriente potassio   = { "Potassio",   PINO_BTN_K, false, false, 0 };

// ---------------------------------------------------------------------------
// ESTADO GLOBAL
// ---------------------------------------------------------------------------
DHT dht(PINO_DHT, TIPO_DHT);

bool  bombaLigada    = false;
float umidadeAtual   = -1.0;   // cache da ultima leitura valida do DHT22
unsigned long ultimaLeitura = 0;
unsigned long ciclo  = 0;

// ---------------------------------------------------------------------------
// LEITURA DOS SENSORES
// ---------------------------------------------------------------------------

// Alterna o estado do nutriente na borda de descida do botao (com debounce).
// Devolve true se o estado mudou neste instante.
bool atualizarNutriente(Nutriente &nut) {
  bool pressionadoAgora = (digitalRead(nut.pino) == LOW);  // PULLUP: LOW = clicado

  if (pressionadoAgora != nut.pressionadoAntes) {
    if (millis() - nut.ultimaMudanca > DEBOUNCE_MS) {
      nut.ultimaMudanca    = millis();
      nut.pressionadoAntes = pressionadoAgora;

      if (pressionadoAgora) {            // acabou de ser pressionado
        nut.presente = !nut.presente;    // alterna o nivel do nutriente
        return true;
      }
    }
  }
  return false;
}

// Converte a leitura analogica do LDR para a escala de pH (0 a 14).
float converterPh(int bruto) {
  float ph = (bruto * PH_ESCALA_MAXIMA) / ADC_MAXIMO;
  if (ph < 0.0) ph = 0.0;
  if (ph > PH_ESCALA_MAXIMA) ph = PH_ESCALA_MAXIMA;
  return ph;
}

// Umidade do solo (no simulador, a umidade do ar lida pelo DHT22)
float lerUmidade() {
  float umidade = dht.readHumidity();
  if (isnan(umidade)) {
    return -1.0;  // sinaliza falha de leitura
  }
  return umidade;
}

// ---------------------------------------------------------------------------
// LOGICA DE DECISAO DA IRRIGACAO
// ---------------------------------------------------------------------------
/*
 * Regras, em ordem de prioridade:
 *
 *  1. Falha de leitura do DHT22        -> bomba DESLIGADA (seguranca).
 *  2. Umidade >= 60%                   -> bomba DESLIGADA (solo saturado).
 *  3. pH fora de 5,0 a 6,8             -> bomba DESLIGADA: o cafeeiro nao
 *                                         absorve nutrientes nessa condicao,
 *                                         entao irrigar so lixivia o adubo.
 *  4. Umidade < 35%                    -> bomba LIGADA (estresse hidrico), o
 *                                         NPK nao importa: salvar a planta vem
 *                                         antes da eficiencia da adubacao.
 *  5. 35% <= Umidade < 50% e N, P e K  -> bomba LIGADA (fertirrigacao): so vale
 *     todos presentes                     gastar agua nessa faixa quando ha
 *                                         nutriente no solo para ser carregado
 *                                         ate as raizes.
 *  6. Demais casos                     -> bomba mantem o estado anterior
 *                                         (histerese entre 50% e 60%).
 */
bool decidirIrrigacao(float umidade, float ph, bool n, bool p, bool k,
                      String &motivo) {
  if (umidade < 0.0) {
    motivo = "Falha na leitura do DHT22 - bomba travada por seguranca";
    return false;
  }

  if (umidade >= UMIDADE_MAXIMA) {
    motivo = "Solo saturado (umidade >= 60%)";
    return false;
  }

  if (ph < PH_MINIMO || ph > PH_MAXIMO) {
    motivo = "pH fora da faixa ideal do cafe (5,0 a 6,8)";
    return false;
  }

  if (umidade < UMIDADE_CRITICA) {
    motivo = "Umidade critica (< 35%) - irrigacao de emergencia";
    return true;
  }

  if (umidade < UMIDADE_PREVENTIVA) {
    if (n && p && k) {
      motivo = "Solo seco com NPK completo - fertirrigacao liberada";
      return true;
    }
    motivo = "Falta nutriente (NPK incompleto) - aguardando adubacao";
    return false;
  }

  // Zona de histerese: entre 50% e 60% mantem o que ja estava acontecendo
  motivo = bombaLigada ? "Histerese: completando a irrigacao ate 60%"
                       : "Umidade suficiente - sem necessidade de irrigar";
  return bombaLigada;
}

// ---------------------------------------------------------------------------
// SAIDA NO MONITOR SERIAL
// ---------------------------------------------------------------------------
void imprimirCabecalho() {
  Serial.println();
  Serial.println("======================================================================");
  Serial.println("  FARMTECH SOLUTIONS - IRRIGACAO INTELIGENTE (CULTURA: CAFE)");
  Serial.println("======================================================================");
  Serial.println("  pH ideal       : 5.0 a 6.8");
  Serial.println("  Umidade minima : 35% (critica) / 50% (preventiva)");
  Serial.println("  Umidade maxima : 60%");
  Serial.println("  Botoes N/P/K   : cada clique ALTERNA o nivel do nutriente");
  Serial.println("======================================================================");
  Serial.println();
  Serial.println("Ciclo |   N |   P |   K |  ADC |    pH | Umidade | Bomba | Motivo");
  Serial.println("------+-----+-----+-----+------+-------+---------+-------+----------------");
}

void imprimirLinha(unsigned long c, int adc, float ph, float umidade,
                   const String &motivo) {
  char linha[180];
  char textoUmidade[12];

  if (umidade < 0.0) {
    snprintf(textoUmidade, sizeof(textoUmidade), "   ERRO");
  } else {
    snprintf(textoUmidade, sizeof(textoUmidade), "%6.1f%%", umidade);
  }

  snprintf(linha, sizeof(linha),
           "%5lu | %3s | %3s | %3s | %4d | %5.2f | %7s | %5s | %s",
           c,
           nitrogenio.presente ? "ON" : "--",
           fosforo.presente    ? "ON" : "--",
           potassio.presente   ? "ON" : "--",
           adc,
           ph,
           textoUmidade,
           bombaLigada ? "LIGA" : "DESL",
           motivo.c_str());

  Serial.println(linha);
}

// ---------------------------------------------------------------------------
// SETUP
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(PINO_BTN_N, INPUT_PULLUP);
  pinMode(PINO_BTN_P, INPUT_PULLUP);
  pinMode(PINO_BTN_K, INPUT_PULLUP);
  pinMode(PINO_RELE, OUTPUT);
  digitalWrite(PINO_RELE, LOW);   // bomba comeca desligada

  analogReadResolution(12);       // 0 a 4095
  dht.begin();

  imprimirCabecalho();
}

// ---------------------------------------------------------------------------
// LOOP PRINCIPAL
// ---------------------------------------------------------------------------
void loop() {
  // -------------------------------------------------------------------------
  // 1. Botoes: lidos a cada volta do loop para a resposta ser imediata
  // -------------------------------------------------------------------------
  bool mudouN = atualizarNutriente(nitrogenio);
  bool mudouP = atualizarNutriente(fosforo);
  bool mudouK = atualizarNutriente(potassio);
  bool houveClique = mudouN || mudouP || mudouK;

  if (houveClique) {
    Serial.printf(">>> NPK alterado: N=%s P=%s K=%s\n",
                  nitrogenio.presente ? "ON" : "OFF",
                  fosforo.presente    ? "ON" : "OFF",
                  potassio.presente   ? "ON" : "OFF");
  }

  // -------------------------------------------------------------------------
  // 2. LDR (pH): leitura continua, tambem para resposta imediata
  // -------------------------------------------------------------------------
  int   adcBruto = analogRead(PINO_LDR);
  float ph       = converterPh(adcBruto);

  // -------------------------------------------------------------------------
  // 3. DHT22 (umidade): leitura a cada 2 s, respeitando o tempo do sensor
  // -------------------------------------------------------------------------
  unsigned long agora = millis();
  bool novaLeitura = (agora - ultimaLeitura >= INTERVALO_LEITURA);

  if (novaLeitura) {
    ultimaLeitura = agora;
    umidadeAtual  = lerUmidade();
    ciclo++;
  }

  // -------------------------------------------------------------------------
  // 4. Decisao e acionamento do rele
  // -------------------------------------------------------------------------
  String motivo;
  bool novoEstado = decidirIrrigacao(umidadeAtual, ph, nitrogenio.presente,
                                     fosforo.presente, potassio.presente,
                                     motivo);

  bool mudouBomba = (novoEstado != bombaLigada);
  if (mudouBomba) {
    bombaLigada = novoEstado;
    digitalWrite(PINO_RELE, bombaLigada ? HIGH : LOW);
    Serial.println(bombaLigada ? ">>> BOMBA D'AGUA LIGADA"
                               : ">>> BOMBA D'AGUA DESLIGADA");
    Serial.printf("    Motivo: %s\n", motivo.c_str());
  }

  // -------------------------------------------------------------------------
  // 5. Telemetria: a cada ciclo de leitura ou quando algo muda
  // -------------------------------------------------------------------------
  if (novaLeitura || houveClique || mudouBomba) {
    imprimirLinha(ciclo, adcBruto, ph, umidadeAtual, motivo);
  }

  // Reimprime o cabecalho a cada 20 ciclos para o monitor nao virar um borrao
  if (novaLeitura && ciclo % 20 == 0) {
    imprimirCabecalho();
  }

  delay(10);  // alivia a CPU sem atrapalhar a leitura dos botoes
}

