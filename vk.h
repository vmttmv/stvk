#ifndef VK_H
#define VK_H

#include <stdint.h>
#include <X11/Xlib.h>

#define COLOREQ(ca, cb)         ((ca).r == (cb).r && \
                                 (ca).g == (cb).g && \
                                 (ca).b == (cb).b)
#define NOCOLOR                 (Color){0}
#define NOUV                    UINT16_MAX

#pragma pack(push, 1)
typedef struct {
        uint16_t x;
        uint16_t y;
        uint16_t w;
        uint16_t h;
} Rect;

typedef struct {
        uint8_t r;
        uint8_t g;
        uint8_t b;
        uint8_t a;
} Color;
#pragma pack(pop)

int blitatlas(uint16_t *, uint16_t *, uint16_t, uint16_t, uint16_t, uint16_t,
                uint16_t, uint16_t, uint16_t, const uint8_t *);

int vkinit(Display *, Window, int, int);
void vkfree(void);
int vkresize(int, int);
void vkpushquad(uint16_t, uint16_t, uint16_t, uint16_t, uint16_t, uint16_t, Color, Color);
int vkflush(void);

#endif
