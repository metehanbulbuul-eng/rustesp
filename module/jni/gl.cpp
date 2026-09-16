#include <GLES3/gl3.h>
#include <vector>
#include <string>
#include "esp.h"

// Basit Shader ve Vertex Buffers için global değişkenler
static GLuint shaderProgram;
static GLuint vbo;
static int screenWidth = 2400, screenHeight = 1080;

const char* vertexShaderSource = R"(
    attribute vec2 position;
    uniform vec2 screenRes;
    void main() {
        vec2 pos = (position / screenRes) * 2.0 - 1.0;
        gl_Position = vec4(pos.x, -pos.y, 0.0, 1.0);
    }
)";

const char* fragmentShaderSource = R"(
    precision mediump float;
    uniform vec4 color;
    void main() {
        gl_FragColor = color;
    }
)";

GLuint compileShader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    return shader;
}

bool gl_init() {
    shaderProgram = glCreateProgram();
    GLuint vs = compileShader(GL_VERTEX_SHADER, vertexShaderSource);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, fragmentShaderSource);
    glAttachShader(shaderProgram, vs);
    glAttachShader(shaderProgram, fs);
    glLinkProgram(shaderProgram);

    glGenBuffers(1, &vbo);
    return true;
}

void gl_set_screen_size(int w, int h) {
    screenWidth = w;
    screenHeight = h;
}

void gl_draw_rect(float x, float y, float w, float h, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    glUseProgram(shaderProgram);
    GLint resLoc = glGetUniformLocation(shaderProgram, "screenRes");
    glUniform2f(resLoc, (float)screenWidth, (float)screenHeight);

    GLint colorLoc = glGetUniformLocation(shaderProgram, "color");
    glUniform4f(colorLoc, r/255.0f, g/255.0f, b/255.0f, a/255.0f);

    float vertices[] = {
        x, y, x + w, y,
        x + w, y, x + w, y + h,
        x + w, y + h, x, y + h,
        x, y + h, x, y
    };

    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_DYNAMIC_DRAW);
    
    GLint posLoc = glGetAttribLocation(shaderProgram, "position");
    glEnableVertexAttribArray(posLoc);
    glVertexAttribPointer(posLoc, 2, GL_FLOAT, GL_FALSE, 0, 0);

    glDrawArrays(GL_LINES, 0, 8);
    glDisableVertexAttribArray(posLoc);
}

void gl_draw_filled_rect(float x, float y, float w, float h, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    glUseProgram(shaderProgram);
    GLint resLoc = glGetUniformLocation(shaderProgram, "screenRes");
    glUniform2f(resLoc, (float)screenWidth, (float)screenHeight);

    GLint colorLoc = glGetUniformLocation(shaderProgram, "color");
    glUniform4f(colorLoc, r/255.0f, g/255.0f, b/255.0f, a/255.0f);

    float vertices[] = {
        x, y,
        x + w, y,
        x, y + h,
        x + w, y + h
    };

    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_DYNAMIC_DRAW);

    GLint posLoc = glGetAttribLocation(shaderProgram, "position");
    glEnableVertexAttribArray(posLoc);
    glVertexAttribPointer(posLoc, 2, GL_FLOAT, GL_FALSE, 0, 0);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(posLoc);
}

void gl_draw_text(float x, float y, const char* text, float scale, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    // Şimdilik yer tutucu, gerçek font render için stbi_truetype veya benzeri lazım
    gl_draw_filled_rect(x, y, 10.0f * scale, 5.0f * scale, r, g, b, a/2);
}

float gl_text_width(const char* text, float scale) {
    if (!text) return 0;
    return strlen(text) * 8.0f * scale;
}
