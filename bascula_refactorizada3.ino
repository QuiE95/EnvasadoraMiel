// ============================================================
// BASCULA CON OLED HELTEC + HX711 + ENCODER
// Versión refactorizada + edición de tarra con EEPROM
// ============================================================

#include <Arduino.h>
#include "HT_SSD1306Wire.h"
#include "HX711.h"
#include <Preferences.h>   // EEPROM estilo ESP32 (NVS)

// ============================================================
// CONSTANTES DE CONFIGURACION
// ============================================================
#define ENC_CLK          4
#define ENC_DT           3
#define ENC_SW           2

#define HX_DOUT         19
#define HX_SCK           5

#define LONG_PRESS_MS  900
#define DEBOUNCE_MS     20
#define ITEMS_VISIBLES   4
#define LIST_Y_INICIO   12
#define LIST_LINE_H     12
#define PESO_UMBRAL  0.03f

#define TARRA_PASO       1    // gramos por paso de encoder
#define TARRA_MIN        0    // valor mínimo permitido
#define TARRA_MAX     9999    // valor máximo permitido

// ============================================================
// STRUCTS
// ============================================================
struct MenuState {
  int op     = 0;
  int inicio = 0;
};

struct MenuItem {
  const char* izq;
  const char* der;
};

// ============================================================
// ESTADOS DE PANTALLA
// ============================================================
enum Pantalla {
  INICIO = 0,
  MANUAL_MENU,
  TARRA_MENU,
  TARRA_EDIT,       // <-- nuevo: editar gramos de la tarra seleccionada
  RELLENO_MENU,
  CTRL2_MENU,
  GRIFO_MENU,
  AUTO_MENU,
  CORRECCION_MENU,
  PARAM_MENU,
  CTRL1_MENU,
  CALIBRAR_MENU,
  RESET_MENU
};

Pantalla estado = INICIO;

// ============================================================
// DISPLAY / HX711 / PREFERENCES
// ============================================================
SSD1306Wire display(0x3c, 500000, SDA_OLED, SCL_OLED, GEOMETRY_128_64, RST_OLED);
HX711       scale;
Preferences prefs;

const float CALIBRATION_FACTOR = 70800.0f;

void VextON() {
  pinMode(Vext, OUTPUT);
  digitalWrite(Vext, LOW);
}

// ============================================================
// DATOS DE TARRA
// nombres de los envases (columna izquierda, fija)
// gramos editables cargados desde EEPROM al arrancar
// ============================================================
const int TOTAL_TARRA = 5;
const char* tarraNombre[] = { "320-V", "550-V", "1250-V", "120-V", "280-V" };
int         tarraGramos[] = {    148,     256,      432,      12,      24  }; // valores por defecto
int         tarraEditTemp = 0;   // valor temporal mientras se edita

MenuState stTarra;

// Construye el arreglo MenuItem dinámicamente para drawList
// (se llama cada vez que se dibuja el menú de tarra)
MenuItem itemsTarraCache[TOTAL_TARRA];
char     tarraGramosStr[TOTAL_TARRA][8]; // buffers para los strings

void refrescarCacheTarra() {
  for (int i = 0; i < TOTAL_TARRA; i++) {
    snprintf(tarraGramosStr[i], sizeof(tarraGramosStr[i]), "%dg", tarraGramos[i]);
    itemsTarraCache[i].izq = tarraNombre[i];
    itemsTarraCache[i].der = tarraGramosStr[i];
  }
}

// ============================================================
// EEPROM — leer y guardar valores de tarra
// ============================================================
void cargarTarraEEPROM() {
  prefs.begin("tarra", true); // true = solo lectura
  for (int i = 0; i < TOTAL_TARRA; i++) {
    char clave[8];
    snprintf(clave, sizeof(clave), "t%d", i);
    tarraGramos[i] = prefs.getInt(clave, tarraGramos[i]); // usa default si no existe
  }
  prefs.end();
}

void guardarTarraEEPROM() {
  prefs.begin("tarra", false); // false = lectura/escritura
  for (int i = 0; i < TOTAL_TARRA; i++) {
    char clave[8];
    snprintf(clave, sizeof(clave), "t%d", i);
    prefs.putInt(clave, tarraGramos[i]);
  }
  prefs.end();
}

// ============================================================
// DATOS DE LOS DEMÁS MENUS (sin cambios)
// ============================================================
const char* menuInicio[] = { "Auto", "Manual" };
const int   totalInicio  = 2;
MenuState   stInicio;

const char* menuManual[] = {
  "Tarra", "Relleno", "Controlador 2", "Grifo Miel",
  "Automatico", "Correccion", "Parametros", "Controlador 1",
  "Calibrar", "RESET ALL"
};
const int totalManual = 10;
MenuState stManual;
int       pasoReset   = 0;

const MenuItem itemsRelleno[] = {
  {"320g","V"}, {"550g","V"}, {"1250g","V"}, {"120g","V"}, {"280g","V"}
};
const int totalRelleno = 5;
MenuState stRelleno;

const MenuItem itemsCtrl2[] = {
  {"320g","1676 pet"}, {"550g","639 pet"}, {"1250g","374 pet"},
  {"120g","204 pet"},  {"280g","208 pet"}
};
const int totalCtrl2 = 5;
MenuState stCtrl2;

const MenuItem itemsGrifo[] = {
  {"Livesetup","OFF"}, {"Minimo","0"}, {"Maximo","30"}, {"Ahorrar","90"}
};
const int totalGrifo = 4;
MenuState stGrifo;

const MenuItem itemsParam[] = {
  {"RIP","On"}, {"Menu","Lista"}, {"save",""}
};
const int totalParam = 3;
MenuState stParam;

const MenuItem itemsCtrl1[] = {
  {"320g","1676 pet"}, {"550g","639 pet"}, {"1250g","374 pet"},
  {"120g","204 pet"},  {"280g","208 pet"}
};
const int totalCtrl1 = 5;
MenuState stCtrl1;

// ============================================================
// ENCODER / BOTON
// ============================================================
int           lastCLK        = HIGH;
bool          lastSW         = HIGH;
unsigned long pressStart     = 0;
bool          buttonHeld     = false;
bool          longPressHandled = false;

// ============================================================
// FUNCIONES DE DIBUJO
// ============================================================

void drawInicio() {
  display.clear();
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.setFont(ArialMT_Plain_16);
  display.drawString(0, 0, "Modo");
  for (int i = 0; i < totalInicio; i++) {
    String linea = (i == stInicio.op) ? "> " : "  ";
    linea += menuInicio[i];
    display.drawString(0, 22 + i * 18, linea);
  }
  display.display();
}

void drawManualMenu() {
  display.clear();
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.setFont(ArialMT_Plain_10);
  display.drawString(0, 0, "MENU MANUAL");
  for (int i = 0; i < ITEMS_VISIBLES; i++) {
    int idx = stManual.inicio + i;
    if (idx >= totalManual) break;
    String linea = (idx == stManual.op) ? "> " : "  ";
    linea += menuManual[idx];
    display.drawString(0, LIST_Y_INICIO + i * LIST_LINE_H, linea);
  }
  display.display();
}


void drawList(const char* titulo, const MenuItem items[], int total, MenuState& st) {
  display.clear();
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.setFont(ArialMT_Plain_10);
  display.drawString(0, 0, titulo);
  for (int i = 0; i < ITEMS_VISIBLES; i++) {
    int idx = st.inicio + i;
    if (idx >= total) break;
    String linea = (idx == st.op) ? "> " : "  ";
    linea += String(items[idx].izq) + "   " + String(items[idx].der);
    display.drawString(0, LIST_Y_INICIO + i * LIST_LINE_H, linea);
  }
  display.display();
}

// Pantalla de edición de gramos de tarra
void drawTarraEdit() {
  display.clear();
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.setFont(ArialMT_Plain_10);

  // Título con el nombre del envase seleccionado
  String titulo = "Tarra: ";
  titulo += tarraNombre[stTarra.op];
  display.drawString(0, 0, titulo);

  // Valor grande en el centro
  display.setFont(ArialMT_Plain_24);
  String val = String(tarraEditTemp) + "g";
  display.drawString(0, 20, val);

  // Instrucciones abajo
  display.setFont(ArialMT_Plain_10);
  display.drawString(0, 52, "OK=guardar  Hold=cancelar");

  display.display();
}

void drawAutoMenu() {
  if (!scale.is_ready()) {
    display.clear();
    display.setFont(ArialMT_Plain_10);
    display.drawString(0, 0, "Sensor no listo");
    display.display();
    return;
  }
  float peso = scale.get_units(20);
  if (peso > -PESO_UMBRAL && peso < PESO_UMBRAL) peso = 0.0f;

  display.clear();
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.setFont(ArialMT_Plain_16);
  display.drawString(0, 0, "Bascula Auto");
  display.setFont(ArialMT_Plain_24);
  display.drawString(0, 20, String(peso, 2));
  display.setFont(ArialMT_Plain_10);
  display.drawString(100, 30, "kg");
  display.drawString(0, 54, "Click=tara");
  display.display();

  Serial.print("Peso: ");
  Serial.println(peso, 2);
}

void drawCorreccion() {
  display.clear();
  display.setFont(ArialMT_Plain_10);
  display.drawString(0, 0,  "Correccion");
  display.drawString(0, 16, "0");
  display.drawString(0, 32, "Valor antiguo");
  display.drawString(0, 48, "0");
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

void drawReset() {
  display.clear();
  display.setFont(ArialMT_Plain_10);
  display.drawString(0, 0,  "BORRAR TODO");
  display.drawString(0, 24, pasoReset == 0 ? "> volver" : "> ok");
  display.display();
}

// ============================================================
// DESPACHADOR PRINCIPAL
// ============================================================
void dibujarPantallas() {
  switch (estado) {
    case INICIO:          drawInicio();                                                        break;
    case MANUAL_MENU:     drawManualMenu();                                                    break;
    case TARRA_MENU:      refrescarCacheTarra();
                          drawList("TARRA", itemsTarraCache, TOTAL_TARRA, stTarra);           break;
    case TARRA_EDIT:      drawTarraEdit();                                                     break;
    case RELLENO_MENU:    drawList("RELLENO", itemsRelleno,  totalRelleno, stRelleno);        break;
    case CTRL2_MENU:      drawList("CTRL 2",  itemsCtrl2,    totalCtrl2,   stCtrl2);          break;
    case GRIFO_MENU:      drawList("GRIFO",   itemsGrifo,    totalGrifo,   stGrifo);          break;
    case AUTO_MENU:       drawAutoMenu();                                                      break;
    case CORRECCION_MENU: drawCorreccion();                                                    break;
    case PARAM_MENU:      drawList("PARAM",   itemsParam,    totalParam,   stParam);          break;
    case CTRL1_MENU:      drawList("CTRL 1",  itemsCtrl1,    totalCtrl1,   stCtrl1);          break;
    case CALIBRAR_MENU:   drawCalibrar();                                                      break;
    case RESET_MENU:      drawReset();                                                         break;
  }
}

// ============================================================
// CONTROL DE LISTAS (encoder girando en menús)
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

// ============================================================
// LECTURA DEL ENCODER
// ============================================================
void leerEncoder() {
  int currentCLK = digitalRead(ENC_CLK);

  if (currentCLK != lastCLK && currentCLK == HIGH) {

    if (estado == TARRA_EDIT) {
      // En modo edición: sube o baja el valor en gramos
      if (digitalRead(ENC_DT) != digitalRead(ENC_CLK)) tarraEditTemp += TARRA_PASO;
      else                                               tarraEditTemp -= TARRA_PASO;

      // Limitar rango
      if (tarraEditTemp < TARRA_MIN) tarraEditTemp = TARRA_MIN;
      if (tarraEditTemp > TARRA_MAX) tarraEditTemp = TARRA_MAX;

    } else {
      // En el resto de menús: navegar lista
      switch (estado) {
        case INICIO:       actualizarLista(stInicio,  totalInicio);  break;
        case MANUAL_MENU:  actualizarLista(stManual,  totalManual);  break;
        case TARRA_MENU:   actualizarLista(stTarra,   TOTAL_TARRA);  break;
        case RELLENO_MENU: actualizarLista(stRelleno, totalRelleno); break;
        case CTRL2_MENU:   actualizarLista(stCtrl2,   totalCtrl2);   break;
        case GRIFO_MENU:   actualizarLista(stGrifo,   totalGrifo);   break;
        case PARAM_MENU:   actualizarLista(stParam,   totalParam);   break;
        case CTRL1_MENU:   actualizarLista(stCtrl1,   totalCtrl1);   break;
        default: break;
      }
    }
  }
  lastCLK = currentCLK;
}

// ============================================================
// NAVEGACION — click corto
// ============================================================
void clickCorto() {
  switch (estado) {

    case INICIO:
      estado = (stInicio.op == 0) ? MANUAL_MENU : AUTO_MENU;
      break;

    case MANUAL_MENU:
      switch (stManual.op) {
        case 0: estado = TARRA_MENU;      break;
        case 1: estado = RELLENO_MENU;    break;
        case 2: estado = CTRL2_MENU;      break;
        case 3: estado = GRIFO_MENU;      break;
        case 4: estado = AUTO_MENU;       break;
        case 5: estado = CORRECCION_MENU; break;
        case 6: estado = PARAM_MENU;      break;
        case 7: estado = CTRL1_MENU;      break;
        case 8: estado = CALIBRAR_MENU;   break;
        case 9:
          estado    = RESET_MENU;
          pasoReset = 0;
          break;
      }
      break;

    case TARRA_MENU:
      // Seleccionó un envase → entrar a editar sus gramos
      tarraEditTemp = tarraGramos[stTarra.op]; // carga valor actual
      estado = TARRA_EDIT;
      break;

    case TARRA_EDIT:
      // Confirmar: guardar valor editado y volver al menú
      tarraGramos[stTarra.op] = tarraEditTemp;
      guardarTarraEEPROM();
      estado = TARRA_MENU;
      break;

    case AUTO_MENU:
      scale.tare();
      break;

    case RESET_MENU:
      if (pasoReset == 0) {
        pasoReset = 1;
      } else {
        estado    = MANUAL_MENU;
        pasoReset = 0;
      }
      break;

    default:
      estado = MANUAL_MENU;
      break;
  }
}

// ============================================================
// NAVEGACION — click largo → subir un nivel / volver a INICIO
// ============================================================
void clickLargo() {
  switch (estado) {
    case TARRA_EDIT:
      // Cancelar edición sin guardar → volver al menú de tarra
      estado = TARRA_MENU;
      break;
    default:
      estado = INICIO;
      break;
  }
}

// ============================================================
// LECTURA DEL BOTON (debounce sin delay)
// ============================================================
void leerBoton() {
  bool          currentSW = digitalRead(ENC_SW);
  unsigned long ahora     = millis();

  if (lastSW == HIGH && currentSW == LOW) {
    pressStart       = ahora;
    buttonHeld       = true;
    longPressHandled = false;
  }

  if (buttonHeld && currentSW == LOW) {
    if (!longPressHandled && (ahora - pressStart > LONG_PRESS_MS)) {
      clickLargo();
      longPressHandled = true;
    }
  }

  if (lastSW == LOW && currentSW == HIGH) {
    if ((ahora - pressStart) >= DEBOUNCE_MS && buttonHeld && !longPressHandled) {
      clickCorto();
    }
    buttonHeld = false;
  }

  lastSW = currentSW;
}

// ============================================================
// SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(200);

  VextON();
  delay(100);

  display.init();
  display.clear();
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.setFont(ArialMT_Plain_10);
  display.drawString(0, 0, "Iniciando...");
  display.display();

  pinMode(ENC_CLK, INPUT_PULLUP);
  pinMode(ENC_DT,  INPUT_PULLUP);
  pinMode(ENC_SW,  INPUT_PULLUP);

  lastCLK = digitalRead(ENC_CLK);
  lastSW  = digitalRead(ENC_SW);

  // Cargar valores de tarra guardados en EEPROM
  cargarTarraEEPROM();

  scale.begin(HX_DOUT, HX_SCK);
  scale.set_scale();
  delay(500);
  scale.tare();
  delay(1000);
  scale.set_scale(CALIBRATION_FACTOR);

  display.clear();
  display.drawString(0, 0, "Sistema listo");
  display.display();
  delay(1000);
}

// ============================================================
// LOOP
// ============================================================
void loop() {
  leerEncoder();
  leerBoton();
  dibujarPantallas();

  delay(estado == AUTO_MENU ? 150 : 5);
}
