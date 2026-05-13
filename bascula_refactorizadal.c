// ============================================================
// BASCULA CON OLED HELTEC + HX711 + ENCODER
// v6 — Ctrl1 y Ctrl2 como contadores independientes de pesaje
// ============================================================

#include <Arduino.h>
#include <SSD1306Wire.h>
#include "HX711.h"
#include <Preferences.h>
#include <ESP32Servo.h>

// ============================================================
// 1. PINES Y CONSTANTES
// ============================================================
#define ENC_CLK          4
#define ENC_DT           3
#define ENC_SW           2
#define HX_DOUT         19
#define HX_SCK           5
#define SERVO_PIN       47   // GPIO 47 — libre, no conflicto con OLED_RST (21)

#define LONG_PRESS_MS    900
#define DEBOUNCE_MS       20
#define ITEMS_VISIBLES     4
#define LIST_Y_INICIO     12
#define LIST_LINE_H       12
#define PESO_UMBRAL    0.03f

#define RELLENO_PASO       1
#define RELLENO_MIN        0
#define RELLENO_MAX     9999

// Detección peso estable (tara y contador)
#define ESTABLE_UMBRAL   0.005f   // diferencia máx entre lecturas
#define ESTABLE_MUESTRAS     8    // lecturas consecutivas necesarias
#define ESTABLE_MINIMO   0.010f   // peso mínimo para contar

// Para detectar que se RETIRÓ el frasco (peso vuelve a cero)
#define RETIRO_UMBRAL    0.020f   // por debajo de esto = frasco retirado

// Pines OLED Heltec WiFi Kit 32 V3
#define OLED_SDA  17
#define OLED_SCL  18
#define OLED_RST  21
#define OLED_ADDR 0x3c
#define VEXT_PIN  36
#define TOTAL_TIPOS 3

// ============================================================
// 2. STRUCTS
// ============================================================
struct MenuState {
  int op     = 0;
  int inicio = 0;
};

struct MenuItem {
  const char* izq;
  const char* der;
};

// Registro de un tipo de frasco en el contador
struct RegistroContador {
  int   cantidad;   // número de frascos pesados
  float pesoTotal;  // suma de pesos netos en kg
  int   tipo;       // índice tipo (0=V,1=P,2=T) — solo para mostrar
};

// ============================================================
// 3. ENUM DE ESTADOS
// ============================================================
enum Pantalla {
  INICIO = 0,
  MANUAL_BASCULA,
  AUTO_ELEGIR_FRASCO,
  AUTO_BASCULA,
  MENU_PRINCIPAL,
  TARRA_MENU,
  TARRA_EDIT,
  RELLENO_MENU,
  RELLENO_EDIT_G,
  RELLENO_EDIT_TIPO,
  CTRL_MENU,        // lista de frascos con conteo (Ctrl1 o Ctrl2)
  CTRL_TOTAL,       // pantalla peso total
  CTRL_OPCIONES,    // Borar / Volver
  GRIFO_SERVO,      // control del servo con encoder
  CALIBRAR_MENU,
  RESET_MENU
};

Pantalla estado     = INICIO;
int      ctrlActivo = 1;   // 1 = Ctrl1, 2 = Ctrl2

// ============================================================
// 4. HARDWARE
// ============================================================
SSD1306Wire display(OLED_ADDR, OLED_SDA, OLED_SCL, GEOMETRY_128_64);
HX711       scale;
Preferences prefs;
Servo       miServo;

const float CALIBRATION_FACTOR = 70800.0f;

// ============================================================
// SERVO — ángulo controlado por encoder en modo GRIFO_MENU
// ============================================================
int  servoAngulo    = 90;   // posición actual (0–180°)
int  servoAnterior  = -1;   // para no escribir si no cambió
const int SERVO_PASO = 1;   // grados por paso de encoder

void VextON() {
  pinMode(VEXT_PIN, OUTPUT);
  digitalWrite(VEXT_PIN, LOW);
}

// ============================================================
// 5. TIPOS DE FRASCO
// ============================================================
const char* TIPO_LETRA[]  = { "V", "P", "T" };
const char* TIPO_NOMBRE[] = { "Vidrio", "Plastico", "Tapa" };

// ============================================================
// 6. DATOS — FRASCOS / TARRA
// ============================================================
const int TOTAL_FRASCOS = 5;

char  frascoBase[TOTAL_FRASCOS][8]         = { "150", "250", "500", "1205", "1960" };
int   frascoTipo[TOTAL_FRASCOS]            = {     0,     1,     2,      1,      0 };
float frascoTarraKg[TOTAL_FRASCOS]         = { 0.0f, 0.0f, 0.0f, 0.029f, 0.0f };
char  frascoNombreCompleto[TOTAL_FRASCOS][12];

int           frascoSeleccionado   = -1;
bool          taraAutoGuardada     = false;
unsigned long tiempoConfirmacion   = 0;
int           tarraEditTipo        = 0;

void reconstruirNombreCompleto(int i) {
  snprintf(frascoNombreCompleto[i], sizeof(frascoNombreCompleto[i]),
           "%s-%s", frascoBase[i], TIPO_LETRA[frascoTipo[i]]);
}
void reconstruirTodosLosNombres() {
  for (int i = 0; i < TOTAL_FRASCOS; i++) reconstruirNombreCompleto(i);
}

void cargarTarraEEPROM() {
  prefs.begin("tarra", true);
  for (int i = 0; i < TOTAL_FRASCOS; i++) {
    char c1[8], c2[8], c3[8];
    snprintf(c1, sizeof(c1), "tk%d", i);
    snprintf(c2, sizeof(c2), "tt%d", i);
    snprintf(c3, sizeof(c3), "tb%d", i);
    frascoTarraKg[i] = prefs.getFloat(c1, frascoTarraKg[i]);
    frascoTipo[i]    = prefs.getInt(c2,   frascoTipo[i]);
    String base      = prefs.getString(c3, String(frascoBase[i]));
    strncpy(frascoBase[i], base.c_str(), sizeof(frascoBase[i]) - 1);
  }
  prefs.end();
}

void guardarTarraFrascoEEPROM(int idx) {
  prefs.begin("tarra", false);
  char c1[8], c2[8], c3[8];
  snprintf(c1, sizeof(c1), "tk%d", idx);
  snprintf(c2, sizeof(c2), "tt%d", idx);
  snprintf(c3, sizeof(c3), "tb%d", idx);
  prefs.putFloat(c1, frascoTarraKg[idx]);
  prefs.putInt(c2,   frascoTipo[idx]);
  prefs.putString(c3, frascoBase[idx]);
  prefs.end();
  reconstruirNombreCompleto(idx);
}

MenuItem  itemsTarraCache[TOTAL_FRASCOS];
char      tarraDerStr[TOTAL_FRASCOS][16];
MenuState stTarra;

void refrescarCacheTarra() {
  for (int i = 0; i < TOTAL_FRASCOS; i++) {
    int gramos = (int)(frascoTarraKg[i] * 1000.0f);
    if (gramos > 0)
      snprintf(tarraDerStr[i], sizeof(tarraDerStr[i]),
               "%s  %dg", TIPO_LETRA[frascoTipo[i]], gramos);
    else
      snprintf(tarraDerStr[i], sizeof(tarraDerStr[i]),
               "%s  falta", TIPO_LETRA[frascoTipo[i]]);
    itemsTarraCache[i].izq = frascoNombreCompleto[i];
    itemsTarraCache[i].der = tarraDerStr[i];
  }
}

// ============================================================
// 7. DATOS — RELLENO
// ============================================================
const int TOTAL_RELLENO = 5;

int rellenoGramos[TOTAL_RELLENO] = { 150, 250, 500, 1205, 1960 };
int rellenoTipo[TOTAL_RELLENO]   = {   0,   1,   2,    1,    0 };
int rellenoEditG    = 0;
int rellenoEditTipo = 0;

MenuItem  itemsRellenoCache[TOTAL_RELLENO];
char      rellenoIzqStr[TOTAL_RELLENO][8];
char      rellenoDerStr[TOTAL_RELLENO][4];
MenuState stRelleno;

void refrescarCacheRelleno() {
  for (int i = 0; i < TOTAL_RELLENO; i++) {
    snprintf(rellenoIzqStr[i], sizeof(rellenoIzqStr[i]), "%dg", rellenoGramos[i]);
    snprintf(rellenoDerStr[i], sizeof(rellenoDerStr[i]), "%s",  TIPO_LETRA[rellenoTipo[i]]);
    itemsRellenoCache[i].izq = rellenoIzqStr[i];
    itemsRellenoCache[i].der = rellenoDerStr[i];
  }
}

void cargarRellenoEEPROM() {
  prefs.begin("relleno", true);
  for (int i = 0; i < TOTAL_RELLENO; i++) {
    char c1[8], c2[8];
    snprintf(c1, sizeof(c1), "rg%d", i);
    snprintf(c2, sizeof(c2), "rt%d", i);
    rellenoGramos[i] = prefs.getInt(c1, rellenoGramos[i]);
    rellenoTipo[i]   = prefs.getInt(c2, rellenoTipo[i]);
  }
  prefs.end();
}

void guardarRellenoEEPROM() {
  prefs.begin("relleno", false);
  for (int i = 0; i < TOTAL_RELLENO; i++) {
    char c1[8], c2[8];
    snprintf(c1, sizeof(c1), "rg%d", i);
    snprintf(c2, sizeof(c2), "rt%d", i);
    prefs.putInt(c1, rellenoGramos[i]);
    prefs.putInt(c2, rellenoTipo[i]);
  }
  prefs.end();
}

// ============================================================
// 8. DATOS — CONTADORES (Ctrl1 y Ctrl2)
// Un registro por cada tipo de frasco
// ============================================================
RegistroContador ctrl1[TOTAL_FRASCOS];
RegistroContador ctrl2[TOTAL_FRASCOS];

// Inicializa un contador completo a cero
void limpiarContador(RegistroContador* ctrl) {
  for (int i = 0; i < TOTAL_FRASCOS; i++) {
    ctrl[i].cantidad  = 0;
    ctrl[i].pesoTotal = 0.0f;
    ctrl[i].tipo      = frascoTipo[i];
  }
}

float totalContador(RegistroContador* ctrl) {
  float t = 0.0f;
  for (int i = 0; i < TOTAL_FRASCOS; i++) t += ctrl[i].pesoTotal;
  return t;
}

int totalJarasContador(RegistroContador* ctrl) {
  int t = 0;
  for (int i = 0; i < TOTAL_FRASCOS; i++) t += ctrl[i].cantidad;
  return t;
}

void guardarContadorEEPROM(RegistroContador* ctrl, int num) {
  String ns = "ctrl" + String(num);
  prefs.begin(ns.c_str(), false);
  for (int i = 0; i < TOTAL_FRASCOS; i++) {
    char ck[8], pk[8];
    snprintf(ck, sizeof(ck), "c%d", i);
    snprintf(pk, sizeof(pk), "p%d", i);
    prefs.putInt(ck,   ctrl[i].cantidad);
    prefs.putFloat(pk, ctrl[i].pesoTotal);
  }
  prefs.end();
}

void cargarContadorEEPROM(RegistroContador* ctrl, int num) {
  String ns = "ctrl" + String(num);
  prefs.begin(ns.c_str(), true);
  for (int i = 0; i < TOTAL_FRASCOS; i++) {
    char ck[8], pk[8];
    snprintf(ck, sizeof(ck), "c%d", i);
    snprintf(pk, sizeof(pk), "p%d", i);
    ctrl[i].cantidad  = prefs.getInt(ck,   0);
    ctrl[i].pesoTotal = prefs.getFloat(pk, 0.0f);
    ctrl[i].tipo      = frascoTipo[i];
  }
  prefs.end();
}

void borrarContadorEEPROM(RegistroContador* ctrl, int num) {
  limpiarContador(ctrl);
  guardarContadorEEPROM(ctrl, num);
}

// Cache drawList para CTRL_MENU
// Columnas: "700g T" | "3  0.0kg"
MenuItem  itemsCtrlCache[TOTAL_FRASCOS];
char      ctrlIzqStr[TOTAL_FRASCOS][12];
char      ctrlDerStr[TOTAL_FRASCOS][16];
MenuState stCtrl;
int       ctrlOpcionFinal = 0; // 0=Borar, 1=Volver en CTRL_OPCIONES

void refrescarCacheCtrl(RegistroContador* ctrl) {
  for (int i = 0; i < TOTAL_FRASCOS; i++) {
    // Izquierda: nombre del frasco + tipo
    snprintf(ctrlIzqStr[i], sizeof(ctrlIzqStr[i]),
             "%sg %s", frascoBase[i], TIPO_LETRA[frascoTipo[i]]);
    // Derecha: cantidad + peso acumulado
    snprintf(ctrlDerStr[i], sizeof(ctrlDerStr[i]),
             "%d  %.1fkg", ctrl[i].cantidad, ctrl[i].pesoTotal);
    itemsCtrlCache[i].izq = ctrlIzqStr[i];
    itemsCtrlCache[i].der = ctrlDerStr[i];
  }
}

// ============================================================
// 9. MENÚS GENERALES
// ============================================================
const char* menuInicio[]    = { "Manual", "Auto", "Menu" };
const int   totalInicio     = 3;
MenuState   stInicio;

const char* menuPrincipal[] = {
  "Tarra", "Relleno", "Ctrl 1", "Ctrl 2",
  "Grifo Servo", "Calibrar", "RESET ALL"
};
const int totalMenuPrincipal = 7;
MenuState stMenu;
int       pasoReset = 0;

// ============================================================
// 10. ENCODER / BOTON
// ============================================================
int           lastCLK          = HIGH;
bool          lastSW           = HIGH;
unsigned long pressStart       = 0;
bool          buttonHeld       = false;
bool          longPressHandled = false;

// ============================================================
// 11. DETECCIÓN PESO ESTABLE
// Usada tanto para guardar tara (AUTO) como para contar pesajes
// ============================================================
float lecturaAnterior   = 0.0f;
int   contadorEstable   = 0;
bool  esperandoRetiro   = false;  // true = espera que retiren el frasco antes de contar otro

void resetearDetectorEstable() {
  lecturaAnterior = 0.0f;
  contadorEstable = 0;
  esperandoRetiro = false;
  taraAutoGuardada = false;
}

// Llamar con el peso BRUTO cada ciclo en AUTO_BASCULA
// Devuelve true cuando acaba de estabilizarse un nuevo peso
bool detectarPesoEstable(float pesoKg) {
  // Si estamos esperando que retiren el frasco, no contar
  if (esperandoRetiro) {
    if (pesoKg < RETIRO_UMBRAL) {
      esperandoRetiro = false;  // frasco retirado, listo para el siguiente
      contadorEstable = 0;
    }
    return false;
  }

  if (pesoKg < ESTABLE_MINIMO) {
    contadorEstable = 0;
    return false;
  }

  float dif = fabs(pesoKg - lecturaAnterior);
  if (dif <= ESTABLE_UMBRAL) contadorEstable++;
  else                        contadorEstable = 0;

  lecturaAnterior = pesoKg;

  if (contadorEstable >= ESTABLE_MUESTRAS) {
    contadorEstable = 0;
    esperandoRetiro = true;   // no volver a contar hasta que retiren
    return true;
  }
  return false;
}

// ============================================================
// 12. FUNCIONES DE DIBUJO — helpers
// ============================================================
void drawList(const char* titulo, const MenuItem items[], int total, MenuState& st) {
  display.clear();
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.setFont(ArialMT_Plain_10);
  display.drawString(0, 0, titulo);
  for (int i = 0; i < ITEMS_VISIBLES; i++) {
    int idx = st.inicio + i;
    if (idx >= total) break;
    String izq = ((idx == st.op) ? "> " : "  ") + String(items[idx].izq);
    display.drawString(0,  LIST_Y_INICIO + i * LIST_LINE_H, izq);
    display.drawString(90, LIST_Y_INICIO + i * LIST_LINE_H, items[idx].der);
  }
  display.display();
}

void drawSimpleMenu(const char* titulo, const char* items[], int total, MenuState& st) {
  display.clear();
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.setFont(ArialMT_Plain_10);
  display.drawString(0, 0, titulo);
  for (int i = 0; i < ITEMS_VISIBLES; i++) {
    int idx = st.inicio + i;
    if (idx >= total) break;
    String linea = ((idx == st.op) ? "> " : "  ") + String(items[idx]);
    display.drawString(0, LIST_Y_INICIO + i * LIST_LINE_H, linea);
  }
  display.display();
}

// ============================================================
// 13. PANTALLAS INDIVIDUALES
// ============================================================
void drawInicio() {
  display.clear();
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.setFont(ArialMT_Plain_16);
  display.drawString(0, 0, "Modo");
  for (int i = 0; i < totalInicio; i++) {
    String linea = ((i == stInicio.op) ? "> " : "  ") + String(menuInicio[i]);
    display.drawString(0, 20 + i * 16, linea);
  }
  display.display();
}

void drawManualBascula() {
  if (!scale.is_ready()) {
    display.clear();
    display.setFont(ArialMT_Plain_10);
    display.drawString(0, 0, "Sensor no listo");
    display.display();
    return;
  }
  float peso = scale.get_units(10);
  if (fabs(peso) < PESO_UMBRAL) peso = 0.0f;

  display.clear();
  display.setTextAlignment(TEXT_ALIGN_LEFT);

  // Grados del servo — pequeño arriba a la izquierda
  display.setFont(ArialMT_Plain_10);
  display.drawString(0, 0, String(servoAngulo) + "\xC2\xB0");

  // Etiqueta MANUAL arriba a la derecha
  display.setTextAlignment(TEXT_ALIGN_RIGHT);
  display.drawString(128, 0, "MANUAL");

  // Peso grande en el centro
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.setFont(ArialMT_Plain_24);
  display.drawString(0, 18, String(peso, 2));
  display.setFont(ArialMT_Plain_10);
  display.drawString(90, 26, "kg");

  display.drawString(0, 54, "enc=servo  Click=tara  Hold=salir");
  display.display();
}

void drawAutoElegirFrasco() {
  display.clear();
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.setFont(ArialMT_Plain_10);
  display.drawString(0, 0, "Elegir frasco:");
  for (int i = 0; i < ITEMS_VISIBLES; i++) {
    int idx = stTarra.inicio + i;
    if (idx >= TOTAL_FRASCOS) break;
    int gramos = (int)(frascoTarraKg[idx] * 1000.0f);
    String izq = ((idx == stTarra.op) ? "> " : "  ") + String(frascoNombreCompleto[idx]);
    display.drawString(0,  LIST_Y_INICIO + i * LIST_LINE_H, izq);
    display.drawString(90, LIST_Y_INICIO + i * LIST_LINE_H,
                       gramos > 0 ? String(gramos) + "g" : "falta");
  }
  display.drawString(0, 54, "OK=usar  Hold=salir");
  display.display();
}

void drawAutoBascula() {
  if (!scale.is_ready()) {
    display.clear();
    display.setFont(ArialMT_Plain_10);
    display.drawString(0, 0, "Sensor no listo");
    display.display();
    return;
  }

  float pesoTotal = scale.get_units(10);
  float tara      = (frascoSeleccionado >= 0) ? frascoTarraKg[frascoSeleccionado] : 0.0f;
  float pesoNeto  = pesoTotal - tara;
  if (fabs(pesoNeto) < PESO_UMBRAL) pesoNeto = 0.0f;

  // Detección de peso estable → tara (primera vez) o conteo
  bool estable = detectarPesoEstable(pesoTotal);

  if (estable) {
    if (!taraAutoGuardada && pesoTotal >= ESTABLE_MINIMO) {
      // Primera estabilización: guardar como tara del frasco
      frascoTarraKg[frascoSeleccionado] = pesoTotal;
      guardarTarraFrascoEEPROM(frascoSeleccionado);
      taraAutoGuardada   = true;
      tiempoConfirmacion = millis();
    } else if (taraAutoGuardada && pesoNeto >= ESTABLE_MINIMO) {
      // Estabilizaciones siguientes: registrar en ambos contadores
      int idx = frascoSeleccionado;
      ctrl1[idx].cantidad++;
      ctrl1[idx].pesoTotal += pesoNeto;
      ctrl1[idx].tipo       = frascoTipo[idx];
      ctrl2[idx].cantidad++;
      ctrl2[idx].pesoTotal += pesoNeto;
      ctrl2[idx].tipo       = frascoTipo[idx];
      guardarContadorEEPROM(ctrl1, 1);
      guardarContadorEEPROM(ctrl2, 2);
      tiempoConfirmacion = millis(); // reusar para mostrar feedback
    }
  }

  bool mostrarConfirm = (millis() - tiempoConfirmacion < 1500);

  display.clear();
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.setFont(ArialMT_Plain_10);

  // Fila 1: nombre frasco
  if (frascoSeleccionado >= 0) {
    String h = String(frascoNombreCompleto[frascoSeleccionado]);
    h += "  "; h += TIPO_NOMBRE[frascoTipo[frascoSeleccionado]];
    display.drawString(0, 0, h);
  } else {
    display.drawString(0, 0, "sin frasco");
  }

  // Fila 2: peso neto grande
  display.setFont(ArialMT_Plain_24);
  display.drawString(0, 12, String(pesoNeto, 2));
  display.setFont(ArialMT_Plain_10);
  display.drawString(92, 20, "kg");

  // Fila 3: feedback o tara
  if (mostrarConfirm) {
    if (!taraAutoGuardada) {
      display.drawString(0, 40, ">> Tara guardada!");
    } else {
      // Mostrar conteo del frasco activo
      int idx = frascoSeleccionado;
      String fb = "Jara #";
      fb += String(ctrl1[idx].cantidad);
      fb += "  ";
      fb += String(ctrl1[idx].pesoTotal, 2);
      fb += "kg";
      display.drawString(0, 40, fb);
    }
  } else {
    String ts = "Tara: ";
    ts += (frascoSeleccionado >= 0)
          ? String((int)(tara * 1000.0f)) + "g"
          : "--";
    display.drawString(0, 40, ts);
  }

  // Fila 4: instrucciones
  display.drawString(0, 54, "Click=cambiar  Hold=salir");
  display.display();
}

// CTRL_MENU: lista con conteo por frasco (imagen 1 y 2)
void drawCtrlMenu() {
  RegistroContador* ctrl = (ctrlActivo == 1) ? ctrl1 : ctrl2;
  refrescarCacheCtrl(ctrl);

  display.clear();
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.setFont(ArialMT_Plain_10);

  String titulo = "CTRL ";
  titulo += String(ctrlActivo);
  display.drawString(0, 0, titulo);

  for (int i = 0; i < ITEMS_VISIBLES; i++) {
    int idx = stCtrl.inicio + i;
    if (idx >= TOTAL_FRASCOS) break;
    String izq = ((idx == stCtrl.op) ? "> " : "  ") + String(itemsCtrlCache[idx].izq);
    display.drawString(0,  LIST_Y_INICIO + i * LIST_LINE_H, izq);
    display.drawString(78, LIST_Y_INICIO + i * LIST_LINE_H, itemsCtrlCache[idx].der);
  }
  display.display();
}

// CTRL_TOTAL: peso total acumulado (imagen 3)
void drawCtrlTotal() {
  RegistroContador* ctrl = (ctrlActivo == 1) ? ctrl1 : ctrl2;
  float total  = totalContador(ctrl);
  int   jaras  = totalJarasContador(ctrl);

  display.clear();
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.setFont(ArialMT_Plain_10);

  String titulo = "Peso Total Ctrl";
  titulo += String(ctrlActivo);
  display.drawString(0, 0, titulo);

  display.setFont(ArialMT_Plain_24);
  display.drawString(0, 14, String(total, 1) + "kg");

  display.setFont(ArialMT_Plain_10);
  String jarasStr = "Jaras: "; jarasStr += String(jaras);
  display.drawString(0, 42, jarasStr);
  display.drawString(0, 54, "Click=opciones  Hold=volver");
  display.display();
}

// CTRL_OPCIONES: Borar / Volver (imagen 4)
void drawCtrlOpciones() {
  display.clear();
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.setFont(ArialMT_Plain_16);

  String titulo = "Ctrl"; titulo += String(ctrlActivo);
  display.drawString(0, 0, titulo);

  if (ctrlOpcionFinal == 0)
    display.drawString(0, 20, "> Borrar");
  else
    display.drawString(0, 20, "  Borar");

  if (ctrlOpcionFinal == 1)
    display.drawString(0, 40, "> Volver");
  else
    display.drawString(0, 40, "  Volver");

  display.display();
}

void drawTarraEdit() {
  display.clear();
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.setFont(ArialMT_Plain_10);
  String titulo = "Frasco: "; titulo += frascoBase[stTarra.op];
  display.drawString(0, 0, titulo);

  display.setFont(ArialMT_Plain_24);
  display.drawString(0, 16, TIPO_LETRA[tarraEditTipo]);
  display.setFont(ArialMT_Plain_16);
  String nuevo = String(frascoBase[stTarra.op]) + "-" + TIPO_LETRA[tarraEditTipo];
  display.drawString(30, 20, nuevo);

  display.setFont(ArialMT_Plain_10);
  display.drawString(0, 40, TIPO_NOMBRE[tarraEditTipo]);
  display.drawString(0, 54, "enc=ciclar  OK=ok  Hold=cancel");
  display.display();
}

void drawRellenoEditG() {
  display.clear();
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.setFont(ArialMT_Plain_10);
  display.drawString(0, 0, "Relleno - gramos:");
  display.setFont(ArialMT_Plain_24);
  display.drawString(0, 14, String(rellenoEditG) + "g");
  display.setFont(ArialMT_Plain_10);
  display.drawString(0, 54, "enc=valor  OK=tipo  Hold=cancel");
  display.display();
}

void drawRellenoEditTipo() {
  display.clear();
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.setFont(ArialMT_Plain_10);
  display.drawString(0, 0, "Relleno - tipo:");
  display.drawString(0, 12, String(rellenoEditG) + "g");
  display.setFont(ArialMT_Plain_24);
  display.drawString(0, 26, TIPO_LETRA[rellenoEditTipo]);
  display.setFont(ArialMT_Plain_16);
  display.drawString(30, 30, TIPO_NOMBRE[rellenoEditTipo]);
  display.setFont(ArialMT_Plain_10);
  display.drawString(0, 54, "enc=ciclar  OK=ok  Hold=cancel");
  display.display();
}

void drawCalibrar() {
  display.clear();
  display.setFont(ArialMT_Plain_10);
  display.drawString(0, 0,  "Calibrar");
  display.drawString(0, 18, "Factor:");
  display.drawString(0, 34, String(CALIBRATION_FACTOR, 1));
  display.drawString(0, 52, "Hold=volver");
  display.display();
}

void drawGrifoServo() {
  display.clear();
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.setFont(ArialMT_Plain_10);
  display.drawString(0, 0, "GRIFO SERVO");

  // Ángulo grande en el centro
  display.setFont(ArialMT_Plain_24);
  display.drawString(0, 14, String(servoAngulo) + "\xC2\xB0");  // °

  // Barra visual de posición (0–180 → 0–108 px)
  int barW = map(servoAngulo, 0, 180, 0, 108);
  display.setFont(ArialMT_Plain_10);
  display.drawString(0, 42, "0");
  display.drawString(118, 42, "180");
  display.drawHorizontalLine(10, 50, 108);          // fondo
  display.fillRect(10, 47, barW, 6);                // relleno

  display.drawString(0, 54, "enc=girar  Hold=salir");
  display.display();
}

void drawReset() {
  display.clear();
  display.setFont(ArialMT_Plain_10);
  display.drawString(0, 0,  "BORRAR TODO");
  display.drawString(0, 24, pasoReset == 0 ? "> volver" : "> ok");
  display.display();
}

// ============================================================
// 14. DESPACHADOR
// ============================================================
void dibujarPantallas() {
  switch (estado) {
    case INICIO:            drawInicio();                                                       break;
    case MANUAL_BASCULA:    drawManualBascula();                                                break;
    case AUTO_ELEGIR_FRASCO:drawAutoElegirFrasco();                                             break;
    case AUTO_BASCULA:      drawAutoBascula();                                                  break;
    case MENU_PRINCIPAL:    drawSimpleMenu("MENU", menuPrincipal, totalMenuPrincipal, stMenu); break;
    case TARRA_MENU:        refrescarCacheTarra();
                            drawList("TARRA", itemsTarraCache, TOTAL_FRASCOS, stTarra);        break;
    case TARRA_EDIT:        drawTarraEdit();                                                    break;
    case RELLENO_MENU:      refrescarCacheRelleno();
                            drawList("RELLENO", itemsRellenoCache, TOTAL_RELLENO, stRelleno);  break;
    case RELLENO_EDIT_G:    drawRellenoEditG();                                                 break;
    case RELLENO_EDIT_TIPO: drawRellenoEditTipo();                                              break;
    case CTRL_MENU:         drawCtrlMenu();                                                     break;
    case CTRL_TOTAL:        drawCtrlTotal();                                                    break;
    case GRIFO_SERVO:       drawGrifoServo();                                                   break;
    case CTRL_OPCIONES:     drawCtrlOpciones();                                                 break;
    case CALIBRAR_MENU:     drawCalibrar();                                                     break;
    case RESET_MENU:        drawReset();                                                        break;
  }
}

// ============================================================
// 15. CONTROL DE LISTAS Y ENCODER
// ============================================================
void actualizarLista(MenuState& st, int total) {
  if (digitalRead(ENC_DT) != digitalRead(ENC_CLK)) st.op++;
  else st.op--;
  if (st.op >= total) st.op = 0;
  if (st.op < 0)      st.op = total - 1;
  if (total > ITEMS_VISIBLES) {
    if (st.op >= st.inicio + ITEMS_VISIBLES) st.inicio = st.op - (ITEMS_VISIBLES - 1);
    if (st.op < st.inicio)                   st.inicio = st.op;
  }
}

int ciclarTipo(int actual) {
  if (digitalRead(ENC_DT) != digitalRead(ENC_CLK)) actual++;
  else actual--;
  if (actual >= TOTAL_TIPOS) actual = 0;
  if (actual < 0)             actual = TOTAL_TIPOS - 1;
  return actual;
}

void leerEncoder() {
  int currentCLK = digitalRead(ENC_CLK);
  if (currentCLK != lastCLK && currentCLK == HIGH) {
    switch (estado) {
      case RELLENO_EDIT_G:
        if (digitalRead(ENC_DT) != digitalRead(ENC_CLK)) rellenoEditG += RELLENO_PASO;
        else                                               rellenoEditG -= RELLENO_PASO;
        if (rellenoEditG < RELLENO_MIN) rellenoEditG = RELLENO_MIN;
        if (rellenoEditG > RELLENO_MAX) rellenoEditG = RELLENO_MAX;
        break;
      case RELLENO_EDIT_TIPO: rellenoEditTipo = ciclarTipo(rellenoEditTipo); break;
      case TARRA_EDIT:        tarraEditTipo   = ciclarTipo(tarraEditTipo);   break;
      case CTRL_OPCIONES:
        ctrlOpcionFinal = (ctrlOpcionFinal == 0) ? 1 : 0;
        break;
      case GRIFO_SERVO:
        if (digitalRead(ENC_DT) != digitalRead(ENC_CLK)) servoAngulo += SERVO_PASO;
        else                                               servoAngulo -= SERVO_PASO;
        servoAngulo = constrain(servoAngulo, 0, 180);
        miServo.write(servoAngulo);
        break;
      case MANUAL_BASCULA:
        if (digitalRead(ENC_DT) != digitalRead(ENC_CLK)) servoAngulo += SERVO_PASO;
        else                                               servoAngulo -= SERVO_PASO;
        servoAngulo = constrain(servoAngulo, 0, 180);
        miServo.write(servoAngulo);
        break;
      case INICIO:            actualizarLista(stInicio, totalInicio);          break;
      case MENU_PRINCIPAL:    actualizarLista(stMenu,   totalMenuPrincipal);   break;
      case TARRA_MENU:        actualizarLista(stTarra,  TOTAL_FRASCOS);        break;
      case AUTO_ELEGIR_FRASCO:actualizarLista(stTarra,  TOTAL_FRASCOS);        break;
      case RELLENO_MENU:      actualizarLista(stRelleno,TOTAL_RELLENO);        break;
      case CTRL_MENU:         actualizarLista(stCtrl,   TOTAL_FRASCOS);        break;
      default: break;
    }
  }
  lastCLK = currentCLK;
}

// ============================================================
// 16. NAVEGACIÓN
// ============================================================
void clickCorto() {
  switch (estado) {
    case INICIO:
      if      (stInicio.op == 0) estado = MANUAL_BASCULA;
      else if (stInicio.op == 1) estado = AUTO_ELEGIR_FRASCO;
      else                       estado = MENU_PRINCIPAL;
      break;

    case MANUAL_BASCULA:
      scale.tare();
      break;

    case AUTO_ELEGIR_FRASCO:
      frascoSeleccionado = stTarra.op;
      resetearDetectorEstable();
      estado = AUTO_BASCULA;
      break;

    case AUTO_BASCULA:
      resetearDetectorEstable();
      estado = AUTO_ELEGIR_FRASCO;
      break;

    case MENU_PRINCIPAL:
      switch (stMenu.op) {
        case 0: estado = TARRA_MENU;   break;
        case 1: estado = RELLENO_MENU; break;
        case 2: ctrlActivo = 1; stCtrl = {0,0}; estado = CTRL_MENU;  break;
        case 3: ctrlActivo = 2; stCtrl = {0,0}; estado = CTRL_MENU;  break;
        case 4: estado = GRIFO_SERVO;  break;
        case 5: estado = CALIBRAR_MENU; break;
        case 6: estado = RESET_MENU; pasoReset = 0; break;
      }
      break;

    case TARRA_MENU:
      tarraEditTipo = frascoTipo[stTarra.op];
      estado = TARRA_EDIT;
      break;

    case TARRA_EDIT:
      frascoTipo[stTarra.op] = tarraEditTipo;
      guardarTarraFrascoEEPROM(stTarra.op);
      estado = TARRA_MENU;
      break;

    case RELLENO_MENU:
      rellenoEditG    = rellenoGramos[stRelleno.op];
      rellenoEditTipo = rellenoTipo[stRelleno.op];
      estado = RELLENO_EDIT_G;
      break;

    case RELLENO_EDIT_G:
      estado = RELLENO_EDIT_TIPO;
      break;

    case RELLENO_EDIT_TIPO:
      rellenoGramos[stRelleno.op] = rellenoEditG;
      rellenoTipo[stRelleno.op]   = rellenoEditTipo;
      guardarRellenoEEPROM();
      estado = RELLENO_MENU;
      break;

    case CTRL_MENU:
      // Click en la lista → ir a total
      estado = CTRL_TOTAL;
      break;

    case CTRL_TOTAL:
      // Click en total → ir a opciones
      ctrlOpcionFinal = 1; // default: Volver
      estado = CTRL_OPCIONES;
      break;

    case CTRL_OPCIONES:
      if (ctrlOpcionFinal == 0) {
        // Zorar: borrar el contador activo
        RegistroContador* ctrl = (ctrlActivo == 1) ? ctrl1 : ctrl2;
        borrarContadorEEPROM(ctrl, ctrlActivo);
        estado = CTRL_MENU;
      } else {
        // Volver a la lista
        estado = CTRL_MENU;
      }
      break;

    case RESET_MENU:
      if (pasoReset == 0) pasoReset = 1;
      else {
        borrarContadorEEPROM(ctrl1, 1);
        borrarContadorEEPROM(ctrl2, 2);
        estado = MENU_PRINCIPAL; pasoReset = 0;
      }
      break;

    default:
      estado = MENU_PRINCIPAL;
      break;
  }
}

void clickLargo() {
  switch (estado) {
    case MANUAL_BASCULA:     estado = INICIO;              break;
    case AUTO_ELEGIR_FRASCO: estado = INICIO;              break;
    case AUTO_BASCULA:       resetearDetectorEstable();
                             estado = AUTO_ELEGIR_FRASCO;  break;
    case MENU_PRINCIPAL:     estado = INICIO;              break;
    case TARRA_MENU:         estado = MENU_PRINCIPAL;      break;
    case TARRA_EDIT:         estado = TARRA_MENU;          break;
    case RELLENO_MENU:       estado = MENU_PRINCIPAL;      break;
    case RELLENO_EDIT_G:     estado = RELLENO_MENU;        break;
    case RELLENO_EDIT_TIPO:  estado = RELLENO_MENU;        break;
    case CTRL_MENU:          estado = MENU_PRINCIPAL;      break;
    case CTRL_TOTAL:         estado = CTRL_MENU;           break;
    case CTRL_OPCIONES:      estado = CTRL_MENU;           break;
    case GRIFO_SERVO:        estado = MENU_PRINCIPAL;      break;
    case CALIBRAR_MENU:      estado = MENU_PRINCIPAL;      break;
    case RESET_MENU:         estado = MENU_PRINCIPAL;      break;
    default:                 estado = INICIO;              break;
  }
}

void leerBoton() {
  bool          currentSW = digitalRead(ENC_SW);
  unsigned long ahora     = millis();
  if (lastSW == HIGH && currentSW == LOW) {
    pressStart = ahora; buttonHeld = true; longPressHandled = false;
  }
  if (buttonHeld && currentSW == LOW) {
    if (!longPressHandled && (ahora - pressStart > LONG_PRESS_MS)) {
      clickLargo(); longPressHandled = true;
    }
  }
  if (lastSW == LOW && currentSW == HIGH) {
    if ((ahora - pressStart) >= DEBOUNCE_MS && buttonHeld && !longPressHandled)
      clickCorto();
    buttonHeld = false;
  }
  lastSW = currentSW;
}

// ============================================================
// 17. SETUP Y LOOP
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("1. Serial OK");

  VextON();
  delay(1000);
  Serial.println("2. VextON OK");

  pinMode(OLED_RST, OUTPUT);
  digitalWrite(OLED_RST, LOW);
  delay(50);
  digitalWrite(OLED_RST, HIGH);
  delay(50);

  display.init();
  delay(200);
  display.displayOn();
  display.clear();
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.setFont(ArialMT_Plain_10);
  display.drawString(0, 0, "Iniciando...");
  display.display();
  Serial.println("3. Display OK");

  pinMode(ENC_CLK, INPUT_PULLUP);
  pinMode(ENC_DT,  INPUT_PULLUP);
  pinMode(ENC_SW,  INPUT_PULLUP);
  lastCLK = digitalRead(ENC_CLK);
  lastSW  = digitalRead(ENC_SW);
  Serial.println("4. Encoder OK");

  cargarTarraEEPROM();
  Serial.println("5. Tarra EEPROM OK");
  cargarRellenoEEPROM();
  Serial.println("6. Relleno EEPROM OK");
  cargarContadorEEPROM(ctrl1, 1);
  cargarContadorEEPROM(ctrl2, 2);
  Serial.println("7. Contadores EEPROM OK");
  reconstruirTodosLosNombres();
  Serial.println("8. Nombres OK");

  scale.begin(HX_DOUT, HX_SCK);
  Serial.println("9. HX711 begin OK");
  scale.set_scale();
  delay(500);
  scale.tare();
  Serial.println("10. HX711 tare OK");
  delay(1000);
  scale.set_scale(CALIBRATION_FACTOR);
  Serial.println("11. HX711 scale OK");

  display.clear();
  display.drawString(0, 0, "Sistema listo");
  display.display();
  Serial.println("12. Display final OK");
  delay(1000);

  // Servo al final para no interferir con el display
  miServo.attach(SERVO_PIN, 500, 2500);
  miServo.write(servoAngulo);
  Serial.println("13. Servo OK — LISTO");

  // Forzar primer dibujo
  dibujarPantallas();
}

void loop() {
  leerEncoder();
  leerBoton();
  dibujarPantallas();
  bool modoPeso = (estado == MANUAL_BASCULA || estado == AUTO_BASCULA);
  delay(modoPeso ? 100 : 5);
}
