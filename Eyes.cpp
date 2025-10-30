#include "Eyes.h"
#include <Arduino.h>

/* ======================= ВНУТРЕННЯЯ СТРУКТУРА ======================= */

struct EyeLayer {
  lv_obj_t* base = nullptr;   // жёлтая “плитка”
  lv_obj_t* core = nullptr;   // белая сердцевина
  int base_w = 0, base_h = 0; // исходные размеры
  int core_w = 0, core_h = 0; // исходные размеры

  // запомним исходную геометрию и центр — чтобы «сжимать» глаз без трансформов
  int cx = 0;   // центр внутри группы
  int cy = 0;
  int baseW = 0, baseH = 0;
  int coreW = 0, coreH = 0;
};

struct EyesState {
  lv_obj_t* group = nullptr;
  EyeLayer left;
  EyeLayer right;

  // стили
  lv_style_t style_base;
  lv_style_t style_core;

  int size = 64;
  int spacing = 36;

  // анимация/таймеры
  uint32_t nextBlinkMs = 0;
  bool     blinking    = false;
  uint32_t blinkStart  = 0;
  uint16_t blinkDur    = 120;   // мс на закрытие и столько же на открытие

  // «дыхание» (пульсация сияния)
  uint32_t breatheStart = 0;
  uint16_t breathePeriod = 2200; // мс
  int8_t glowBase = 60;  // 0..100 базовая сила сияния
  int8_t glowAdd  = 0;   // добавка внешним API

  // взгляд
  float lookX = 0.f, lookY = 0.f; // [-1..1]

  // цвета
  lv_color_t colGlow  = lv_color_hex(0xFFC21C);
  lv_color_t colInner = lv_color_hex(0xFFFDE7);

  bool ready = false;
} S;

/* ======================= ВСПОМОГАТЕЛЬНЫЕ ======================= */

static void style_init_if_needed() {
  // base
  lv_style_init(&S.style_base);
  lv_style_set_bg_opa(&S.style_base, LV_OPA_COVER);
  lv_style_set_radius(&S.style_base, LV_RADIUS_CIRCLE);
  lv_style_set_outline_width(&S.style_base, 0);
  lv_style_set_border_width(&S.style_base, 0);
  lv_style_set_bg_color(&S.style_base, S.colGlow);
  lv_style_set_bg_grad_color(&S.style_base, S.colInner);
  lv_style_set_bg_grad_dir(&S.style_base, LV_GRAD_DIR_VER);
  lv_style_set_shadow_color(&S.style_base, S.colGlow);
  lv_style_set_shadow_width(&S.style_base, 28);
  lv_style_set_shadow_spread(&S.style_base, 2);
  lv_style_set_shadow_opa(&S.style_base, LV_OPA_70);

  // core
  lv_style_init(&S.style_core);
  lv_style_set_bg_opa(&S.style_core, LV_OPA_COVER);
  lv_style_set_radius(&S.style_core, LV_RADIUS_CIRCLE);
  lv_style_set_bg_color(&S.style_core, S.colInner);
  lv_style_set_border_width(&S.style_core, 0);
  lv_style_set_outline_width(&S.style_core, 0);
}

static lv_obj_t* make_eye(EyeLayer& eye, lv_obj_t* parent, int x, int y, int w) {
  int h = (int)(w * 0.86f);
  int r = (int)(w * 0.28f);

  eye.base = lv_obj_create(parent);
  lv_obj_remove_style_all(eye.base);
  lv_obj_add_style(eye.base, &S.style_base, 0);
  lv_obj_set_size(eye.base, w, h);
  eye.base_w = w; eye.base_h = h;          // <— сохраняем

  lv_obj_set_style_radius(eye.base, r, 0);
  lv_obj_set_pos(eye.base, x - w/2, y - h/2);

  eye.core = lv_obj_create(eye.base);
  lv_obj_remove_style_all(eye.core);
  lv_obj_add_style(eye.core, &S.style_core, 0);
  int cw = (int)(w * 0.66f);
  int ch = (int)(h * 0.66f);
  lv_obj_set_size(eye.core, cw, ch);
  eye.core_w = cw; eye.core_h = ch;        // <— сохраняем
  lv_obj_center(eye.core);

  // блик
  lv_obj_set_style_shadow_color(eye.core, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_shadow_width(eye.core, 14, 0);
  lv_obj_set_style_shadow_opa(eye.core, LV_OPA_40, 0);
  return eye.base;
}

static void schedule_next_blink() {
  uint16_t rnd = (uint16_t)(1500 + (esp_random() % 2500)); // 1.5..4.0 c
  S.nextBlinkMs = millis() + rnd;
}

// «сжатие века»: без transform-API — меняем высоту виджета и ядра, держа центр.
static void set_eye_squash(EyeLayer& eye, int hPercent) {
  // hPercent: 100 — открыто, ~12 — почти закрыто
  hPercent = constrain(hPercent, 8, 100);

  // целевая высота
  int new_h  = (eye.base_h * hPercent) / 100;
  int new_ch = (eye.core_h * hPercent) / 100;

  // держим центр, поэтому переустанавливаем позицию
  lv_area_t a;
  lv_obj_get_coords(eye.base, &a);
  int cx = (a.x1 + a.x2) / 2;
  int cy = (a.y1 + a.y2) / 2;

  lv_obj_set_size(eye.base, eye.base_w, new_h);
  lv_obj_set_pos(eye.base, cx - eye.base_w/2, cy - new_h/2);

  // ядро уменьшаем по Y так же и центрируем
  lv_obj_set_size(eye.core, eye.core_w, new_ch);
  lv_obj_center(eye.core);
}

static void apply_look_offset(EyeLayer& eye, float nx, float ny) {
  // смещаем ядро в пределах ~16% ширины
  const int maxShift = (int)(S.size * 0.16f);
  int dx = (int)(nx * maxShift);
  int dy = (int)(ny * maxShift);
  lv_obj_set_style_translate_x(eye.core, dx, 0);
  lv_obj_set_style_translate_y(eye.core, dy, 0);
}

static void update_breath() {
  uint32_t t = (millis() - S.breatheStart) % S.breathePeriod;
  float ph = (float)t / (float)S.breathePeriod;
  float s = 0.5f + 0.5f * sinf(ph * 2.f * 3.1415926f); // 0..1

  int base = S.glowBase + S.glowAdd; // 0..100
  base = constrain(base, 0, 100);
  int width = (int)(16 + (12 * s) + (base * 0.18f));
  int opa   = (int)(120 + 80 * s); // 120..200

  lv_obj_set_style_shadow_width(S.left.base,  width, 0);
  lv_obj_set_style_shadow_opa  (S.left.base,  opa,   0);
  lv_obj_set_style_shadow_width(S.right.base, width, 0);
  lv_obj_set_style_shadow_opa  (S.right.base, opa,   0);
}

static void update_blink() {
  uint32_t now = millis();
  if (!S.blinking) {
    if (now >= S.nextBlinkMs) { S.blinking = true; S.blinkStart = now; }
    return;
  }

  float ph = (float)(now - S.blinkStart) / (float)(S.blinkDur * 2); // 0..1
  if (ph >= 1.f) {
    S.blinking = false;
    set_eye_squash(S.left,  100);
    set_eye_squash(S.right, 100);
    schedule_next_blink();
    return;
  }

  if (ph < 0.5f) {
    int h = 100 - (int)(ph / 0.5f * 88.f); // закрытие 100->12
    set_eye_squash(S.left,  h);
    set_eye_squash(S.right, h);
  } else {
    float p = (ph - 0.5f) / 0.5f;
    int h = 12 + (int)(p * 88.f);         // открытие 12->100
    set_eye_squash(S.left,  h);
    set_eye_squash(S.right, h);
  }
}

/* ======================= ПУБЛИЧНОЕ API ======================= */

void Eyes_Create(lv_obj_t* parent, int cx, int cy, int spacing, int size) {
  if (!parent) parent = lv_scr_act();
  S.size = size;
  S.spacing = spacing;

  style_init_if_needed();

  S.group = lv_obj_create(parent);
  lv_obj_remove_style_all(S.group);
  lv_obj_set_size(S.group, size*2 + spacing + 8, (int)(size*0.9f) + 8);
  lv_obj_set_pos(S.group,
                 cx - lv_obj_get_width (S.group)/2,
                 cy - lv_obj_get_height(S.group)/2);

  int leftX  = lv_obj_get_width(S.group)/2 - spacing/2 - size/2;
  int rightX = lv_obj_get_width(S.group)/2 + spacing/2 + size/2;
  int y      = lv_obj_get_height(S.group)/2;

  make_eye(S.left,  S.group, leftX,  y, size);
  make_eye(S.right, S.group, rightX, y, size);

  S.lookX = 0.f; S.lookY = 0.f;
  apply_look_offset(S.left,  0.f, 0.f);
  apply_look_offset(S.right, 0.f, 0.f);

  S.breatheStart = millis();
  schedule_next_blink();
  S.ready = true;
}

void Eyes_Update() {
  if (!S.ready) return;
  update_breath();
  update_blink();
  apply_look_offset(S.left,  S.lookX, S.lookY);
  apply_look_offset(S.right, S.lookX, S.lookY);
}

void Eyes_Look(float nx, float ny) {
  if (!S.ready) return;
  S.lookX = constrain(nx, -1.f, 1.f);
  S.lookY = constrain(ny, -1.f, 1.f);
}

void Eyes_BlinkNow() {
  if (!S.ready) return;
  S.blinking = true;
  S.blinkStart = millis();
}

void Eyes_SetColors(lv_color_t inner, lv_color_t glow) {
  if (!S.ready) { S.colInner = inner; S.colGlow = glow; }
  lv_style_set_bg_color(&S.style_base, glow);
  lv_style_set_bg_grad_color(&S.style_base, inner);
  lv_style_set_shadow_color(&S.style_base, glow);
  lv_obj_report_style_change(&S.style_base);

  lv_style_set_bg_color(&S.style_core, inner);
  lv_obj_report_style_change(&S.style_core);
}

void Eyes_SetGlow(int8_t percent) {
  S.glowAdd = constrain((int)percent, -60, 40);
}
