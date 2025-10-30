#pragma once
#include <lvgl.h>
#include <stdint.h>

/** Публичное API светящихся глаз **/

// Инициализация пары глаз.
// cx, cy — центр всей группы; spacing — расстояние между глазами (px); size — базовая ширина глаза (px).
void Eyes_Create(lv_obj_t* parent, int cx, int cy, int spacing = 36, int size = 64);

// Обновление анимаций (мигание/«дыхание») — вызывать в loop каждый тик.
void Eyes_Update();

// Взгляд: nx, ny в диапазоне [-1..1] (влево/вправо, вверх/вниз).
void Eyes_Look(float nx, float ny);

// Принудительное моргание.
void Eyes_BlinkNow();

// Смена палитры: inner — центр свечения, glow — обводка/сияние.
void Eyes_SetColors(lv_color_t inner, lv_color_t glow);

// Дополнительная яркость сияния [0..100] (поверх базового «дыхания»).
void Eyes_SetGlow(int8_t percent);
