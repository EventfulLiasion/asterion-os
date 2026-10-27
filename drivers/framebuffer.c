#include <stdint.h>
#include <stddef.h>
#include "../include/boot_info.h"
#include "serial.h"
#include "framebuffer.h"

static volatile uint8_t *framebuffer;
static uint32_t screen_w, screen_h, stride, pixel_format;
static uint32_t text_color = 0xD9E7E2, dim_color = 0x81928C, accent_color = 0x91E0B7;
static char lines[56][120];
static uint32_t line_count, scroll_offset;
static int console_ready;

static char console_upper(char c) { return c >= 'a' && c <= 'z' ? (char)(c - 'a' + 'A') : c; }
static void copystr(char *d, const char *s, size_t n) { size_t i=0; while(i+1<n&&s[i]){d[i]=s[i];i++;} if(n)d[i]=0; }
static int str_starts(const char *a,const char *b) { while(*b){if(*a++!=*b++)return 0;}return 1; }

static const uint8_t letters[26][5] = {
 {0x7e,0x11,0x11,0x11,0x7e},{0x7f,0x49,0x49,0x49,0x36},{0x3e,0x41,0x41,0x41,0x22},
 {0x7f,0x41,0x41,0x22,0x1c},{0x7f,0x49,0x49,0x49,0x41},{0x7f,0x09,0x09,0x09,0x01},
 {0x3e,0x41,0x49,0x49,0x7a},{0x7f,0x08,0x08,0x08,0x7f},{0x00,0x41,0x7f,0x41,0x00},
 {0x20,0x40,0x41,0x3f,0x01},{0x7f,0x08,0x14,0x22,0x41},{0x7f,0x40,0x40,0x40,0x40},
 {0x7f,0x02,0x0c,0x02,0x7f},{0x7f,0x04,0x08,0x10,0x7f},{0x3e,0x41,0x41,0x41,0x3e},
 {0x7f,0x09,0x09,0x09,0x06},{0x3e,0x41,0x51,0x21,0x5e},{0x7f,0x09,0x19,0x29,0x46},
 {0x46,0x49,0x49,0x49,0x31},{0x01,0x01,0x7f,0x01,0x01},{0x3f,0x40,0x40,0x40,0x3f},
 {0x1f,0x20,0x40,0x20,0x1f},{0x3f,0x40,0x38,0x40,0x3f},{0x63,0x14,0x08,0x14,0x63},
 {0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43}
};
static const uint8_t digits[10][5] = {
 {0x3e,0x51,0x49,0x45,0x3e},{0x00,0x42,0x7f,0x40,0x00},{0x42,0x61,0x51,0x49,0x46},
 {0x21,0x41,0x45,0x4b,0x31},{0x18,0x14,0x12,0x7f,0x10},{0x27,0x45,0x45,0x45,0x39},
 {0x3c,0x4a,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},{0x36,0x49,0x49,0x49,0x36},
 {0x06,0x49,0x49,0x29,0x1e}
};

static void pixel(uint32_t x,uint32_t y,uint32_t color) {
    if(x>=screen_w||y>=screen_h)return;
    volatile uint8_t *p=framebuffer+((uint64_t)y*stride+x)*4;
    if(pixel_format==0){p[0]=(uint8_t)(color>>16);p[1]=(uint8_t)(color>>8);p[2]=(uint8_t)color;p[3]=0;}
    else {p[0]=(uint8_t)color;p[1]=(uint8_t)(color>>8);p[2]=(uint8_t)(color>>16);p[3]=0;}
}
static void rect(uint32_t x,uint32_t y,uint32_t w,uint32_t h,uint32_t c) {
    if(x>=screen_w||y>=screen_h)return;
    if(w>screen_w-x)w=screen_w-x;
    if(h>screen_h-y)h=screen_h-y;
    for(uint32_t yy=y;yy<y+h;yy++)for(uint32_t xx=x;xx<x+w;xx++)pixel(xx,yy,c);
}
static const uint8_t *glyph(char c) {
    static uint8_t g[5]; c=console_upper(c); if(c>='A'&&c<='Z')return letters[(uint32_t)(c-'A')]; if(c>='0'&&c<='9')return digits[(uint32_t)(c-'0')];
    for(int i=0;i<5;i++)g[i]=0;
    switch(c){case '.':g[2]=0x60;break;case ',':g[2]=0x60;g[3]=0x40;break;case ':':g[2]=0x36;break;case ';':g[2]=0x36;g[3]=0x40;break;case '-':g[1]=8;g[2]=8;g[3]=8;break;case '_':g[0]=0x40;g[1]=0x40;g[2]=0x40;g[3]=0x40;g[4]=0x40;break;case '/':g[0]=0x60;g[1]=0x18;g[2]=6;g[3]=1;break;case '\\':g[0]=3;g[1]=0x0c;g[2]=0x30;g[3]=0x40;break;case '>':g[0]=0x41;g[1]=0x22;g[2]=0x14;g[3]=8;break;case '<':g[0]=8;g[1]=0x14;g[2]=0x22;g[3]=0x41;break;case '=':g[1]=0x14;g[2]=0x14;g[3]=0x14;break;case '+':g[2]=0x1c;g[0]=g[1]=g[3]=g[4]=8;break;case '*':g[1]=0x14;g[2]=8;g[3]=0x14;break;case '(':g[1]=0x1c;g[2]=0x22;g[3]=0x41;break;case ')':g[1]=0x41;g[2]=0x22;g[3]=0x1c;break;case '[':g[1]=0x7f;g[2]=0x41;g[3]=0x41;break;case ']':g[1]=0x41;g[2]=0x41;g[3]=0x7f;break;case '!':g[2]=0x5f;break;case '?':g[1]=2;g[2]=0x51;g[3]=9;g[4]=6;break;case '#':g[0]=0x14;g[1]=0x7f;g[2]=0x14;g[3]=0x7f;g[4]=0x14;break;case '%':g[0]=0x63;g[1]=0x13;g[2]=8;g[3]=0x64;g[4]=0x63;break;case '\'':g[2]=3;g[3]=1;break;case '"':g[1]=3;g[3]=3;break;case '|':g[2]=0x7f;break;case '^':g[1]=2;g[2]=1;g[3]=2;break;case '~':g[1]=4;g[2]=2;g[3]=4;break;default:break;}return g;
}
static void draw_char(int x,int y,char c,uint32_t color,int scale) {
    const uint8_t *g=glyph(c);for(int col=0;col<5;col++)for(int row=0;row<7;row++)if(g[col]&(1u<<row))rect((uint32_t)(x+col*scale),(uint32_t)(y+row*scale),(uint32_t)scale,(uint32_t)scale,color);
}
static void draw_text(int x,int y,const char *s,uint32_t color,int scale) { while(*s){if(*s=='\n'){y+=18*scale;x=24;}else draw_char(x,y,*s,color,scale);x+=7*scale;s++;} }
void console_log(serial_level_t level, const char *s) {
    serial_log(level, s);
    while(*s){if(line_count>=55){for(uint32_t i=1;i<55;i++)copystr(lines[i-1],lines[i],120);line_count=54;}size_t n=0;while(s[n]&&n<112){lines[line_count][n]=s[n];n++;}lines[line_count][n]=0;line_count++;s+=n;}
    if(line_count==0){lines[0][0]=0;line_count=1;}
}
void console_render(const char *input, int shifted) {
    if (!console_ready || !input) return;
    size_t input_len = 0; while (input[input_len] && input_len < 119) ++input_len;
    rect(0,0,screen_w,screen_h,0x0B1218);
    rect(0,0,screen_w,48,0x111D23);rect(20,15,16,16,accent_color);
    draw_text(46,15,"ASTERION OS",text_color,2);draw_text(230,18,"KERNEL MODE  /  X86_64",dim_color,1);
    const char *state=shifted?"SHIFT":"READY";draw_text((int)screen_w-116,18,state,accent_color,1);
    rect(20,62,screen_w-40,2,0x24353B);
    draw_text(24,75,"SYSTEM CONSOLE",accent_color,1);
    uint32_t cols=(screen_w>800?104:70);uint32_t maxlines=(screen_h>620?27:18);uint32_t start=line_count>maxlines?line_count-maxlines:0;
    if(scroll_offset&&line_count>maxlines)start=line_count-maxlines-scroll_offset;
    uint32_t y=101;for(uint32_t i=start;i<line_count&&i<start+maxlines;i++,y+=19){uint32_t c=(str_starts(lines[i],"ASTERION")||str_starts(lines[i],"READY")||str_starts(lines[i],"OK"))?accent_color:text_color;draw_text(24,(int)y,lines[i],c,1);}
    uint32_t prompt_y=screen_h-73;rect(20,prompt_y-11,screen_w-40,1,0x24353B);
    draw_text(24,(int)prompt_y,"ASTERION",accent_color,1);draw_text(101,(int)prompt_y,"::",dim_color,1);
    draw_text(126,(int)prompt_y,input,text_color,1);rect(126+input_len*7,prompt_y+12,5,2,accent_color);
    rect(20,screen_h-30,screen_w-40,1,0x24353B);
    draw_text(24,(int)screen_h-22,"HELP  COMMANDS",dim_color,1);
    draw_text((int)screen_w-330,(int)screen_h-22,"PS/2 INPUT  |  DIRECT FRAMEBUFFER",dim_color,1);
    (void)cols;
}

void console_init(const boot_info_t *info) {
    framebuffer=(volatile uint8_t *)(uintptr_t)info->framebuffer_base;
    screen_w=info->framebuffer_width; screen_h=info->framebuffer_height;
    stride=info->framebuffer_stride; pixel_format=info->pixel_format;
    console_ready=1;
}

void console_clear(void) { line_count=0; scroll_offset=0; }

int console_set_theme(char theme) {
    switch (console_upper(theme)) {
    case 'G': accent_color=0x91E0B7; text_color=0xD9E7E2; return 1;
    case 'A': accent_color=0xF3B66D; text_color=0xE9E3D7; return 1;
    case 'B': accent_color=0x8CC4FF; text_color=0xE0E9F2; return 1;
    default: return 0;
    }
}
