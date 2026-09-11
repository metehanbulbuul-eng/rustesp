#include "esp.h"
#include "font.h"
#include <GLES3/gl3.h>
#include <EGL/egl.h>
#include <cstring>
#include <cmath>

// === Basit gölgelendirici programı ===
static GLuint g_program = 0;
static GLint  g_uMVP = -1;
static GLint  g_uColor = -1;

static const char* VERTEX_SHADER =
    "#version 300 es\n"
    "uniform mat4 uMVP;\n"
    "in vec2 aPos;\n"
    "void main() {\n"
    "    gl_Position = uMVP * vec4(aPos, 0.0, 1.0);\n"
    "}\n";

static const char* FRAGMENT_SHADER =
    "#version 300 es\n"
    "precision mediump float;\n"
    "uniform vec4 uColor;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    "    fragColor = uColor;\n"
    "}\n";

// === Gölgelendirici derleme yardımcısı ===
static GLuint compile_shader(GLenum type, const char* src) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        LOGE("Shader hata: %s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

// === OpenGL başlat ===
bool gl_init() {
    GLuint vs = compile_shader(GL_VERTEX_SHADER, VERTEX_SHADER);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, FRAGMENT_SHADER);
    if (!vs || !fs) return false;

    g_program = glCreateProgram();
    glAttachShader(g_program, vs);
    glAttachShader(g_program, fs);
    glLinkProgram(g_program);

    GLint ok = 0;
    glGetProgramiv(g_program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(g_program, sizeof(log), nullptr, log);
        LOGE("Program link hata: %s", log);
        return false;
    }

    glDeleteShader(vs);
    glDeleteShader(fs);

    g_uMVP = glGetUniformLocation(g_program, "uMVP");
    g_uColor = glGetUniformLocation(g_program, "uColor");

    LOGI("OpenGL hazır. Program=%u", g_program);
    return true;
}

// === Ekran boyutları (oyun başladığında ayarlanacak) ===
static int g_screen_w = 1080;
static int g_screen_h = 2400;

void gl_set_screen_size(int w, int h) {
    g_screen_w = w;
    g_screen_h = h;
}

// === Ortho projeksiyon matrisi (sol üst köşe 0,0) ===
static void ortho_matrix(float* m, float left, float right, float bottom, float top) {
    std::memset(m, 0, 16 * sizeof(float));
    m[0]  = 2.0f / (right - left);
    m[5]  = 2.0f / (top - bottom);
    m[10] = -1.0f;
    m[12] = -(right + left) / (right - left);
    m[13] = -(top + bottom) / (top - bottom);
    m[15] = 1.0f;
}

// === Renk ayarla (0-255) ===
static void set_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    glUniform4f(g_uColor, r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
}

// === İçi boş dikdörtgen çiz ===
void gl_draw_rect(float x, float y, float w, float h,
                  uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    float m[16];
    ortho_matrix(m, 0, g_screen_w, g_screen_h, 0);

    glUseProgram(g_program);
    glUniformMatrix4fv(g_uMVP, 1, GL_FALSE, m);
    set_color(r, g, b, a);

    float verts[8] = {
        x,     y,
        x + w, y,
        x + w, y + h,
        x,     y + h
    };

    GLuint vbo;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);

    GLint aPos = glGetAttribLocation(g_program, "aPos");
    glEnableVertexAttribArray(aPos);
    glVertexAttribPointer(aPos, 2, GL_FLOAT, GL_FALSE, 0, nullptr);

    glDrawArrays(GL_LINE_LOOP, 0, 4);

    glDisableVertexAttribArray(aPos);
    glDeleteBuffers(1, &vbo);
}

// === Çizgi çiz ===
void gl_draw_line(float x1, float y1, float x2, float y2,
                  uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    float m[16];
    ortho_matrix(m, 0, g_screen_w, g_screen_h, 0);

    glUseProgram(g_program);
    glUniformMatrix4fv(g_uMVP, 1, GL_FALSE, m);
    set_color(r, g, b, a);

    float verts[4] = { x1, y1, x2, y2 };

    GLuint vbo;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);

    GLint aPos = glGetAttribLocation(g_program, "aPos");
    glEnableVertexAttribArray(aPos);
    glVertexAttribPointer(aPos, 2, GL_FLOAT, GL_FALSE, 0, nullptr);

    glDrawArrays(GL_LINES, 0, 2);

    glDisableVertexAttribArray(aPos);
    glDeleteBuffers(1, &vbo);
}

// === Dolu dikdörtgen çiz (can barı için) ===
void gl_draw_filled_rect(float x, float y, float w, float h,
                         uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    float m[16];
    ortho_matrix(m, 0, g_screen_w, g_screen_h, 0);

    glUseProgram(g_program);
    glUniformMatrix4fv(g_uMVP, 1, GL_FALSE, m);
    set_color(r, g, b, a);

    float verts[8] = {
        x,     y,
        x + w, y,
        x + w, y + h,
        x,     y + h
    };

    GLuint vbo;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);

    GLint aPos = glGetAttribLocation(g_program, "aPos");
    glEnableVertexAttribArray(aPos);
    glVertexAttribPointer(aPos, 2, GL_FLOAT, GL_FALSE, 0, nullptr);

    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);

    glDisableVertexAttribArray(aPos);
    glDeleteBuffers(1, &vbo);
}

// === Tek karakter çiz (8x8 bitmap) ===
void gl_draw_char(float x, float y, char c, float scale,
                  uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    float m[16];
    ortho_matrix(m, 0, g_screen_w, g_screen_h, 0);

    glUseProgram(g_program);
    glUniformMatrix4fv(g_uMVP, 1, GL_FALSE, m);
    set_color(r, g, b, a);

    GLint aPos = glGetAttribLocation(g_program, "aPos");
    glEnableVertexAttribArray(aPos);

    for (int row = 0; row < 8; row++) {
        uint8_t bits = font_get_row(c, row);
        for (int col = 0; col < 8; col++) {
            if (bits & (1 << col)) {
                float px = x + col * scale;
                float py = y + row * scale;
                float verts[8] = {
                    px,           py,
                    px + scale,   py,
                    px + scale,   py + scale,
                    px,           py + scale
                };

                GLuint vbo;
                glGenBuffers(1, &vbo);
                glBindBuffer(GL_ARRAY_BUFFER, vbo);
                glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);
                glVertexAttribPointer(aPos, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
                glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
                glDeleteBuffers(1, &vbo);
            }
        }
    }

    glDisableVertexAttribArray(aPos);
}

// === Yazı çiz ===
void gl_draw_text(float x, float y, const char* text, float scale,
                  uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (!text) return;
    float cx = x;
    float cy = y;
    for (const char* p = text; *p; p++) {
        if (*p == '\n') {
            cx = x;
            cy += 8 * scale + 2;
            continue;
        }
        gl_draw_char(cx, cy, *p, scale, r, g, b, a);
        cx += 8 * scale;
    }
}

// === Yazı genişliği hesapla ===
float gl_text_width(const char* text, float scale) {
    if (!text) return 0;
    return strlen(text) * 8 * scale;
}