#include "VideoWidget.h"
#include <QOpenGLBuffer>
#include <QDebug>
#include <QOpenGLPixelTransferOptions>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
}

// ===================================================================
// ==                       着色器代码 (Shader)                     ==
// ===================================================================
static const char* g_vertexShaderSource =
"#version 330 core\n"
"layout (location = 0) in vec2 vertexIn;\n"
"layout (location = 1) in vec2 textureIn;\n"
"uniform vec2 scale;\n"
"out vec2 textureOut;\n"
"void main()\n"
"{\n"
"    gl_Position = vec4(vertexIn * scale, 0.0, 1.0);\n"
"    textureOut = textureIn;\n"
"}\n";

// --- 使用能正确处理 Limited Range YUV (BT.601 standard) 的片元着色器 ---
static const char* g_fragmentShaderSource =
"#version 330 core\n"
"in vec2 textureOut;\n"
"uniform sampler2D tex_y;\n"
"uniform sampler2D tex_u;\n"
"uniform sampler2D tex_v;\n"
"out vec4 fragColor;\n"
"void main()\n"
"{\n"
"    float y = texture(tex_y, textureOut).r;\n"
"    float u = texture(tex_u, textureOut).r;\n"
"    float v = texture(tex_v, textureOut).r;\n"
"\n"
"    // 从归一化纹理坐标转换回YUV值\n"
"    y = 1.164 * (y - 0.0625);\n" // 0.0625 is 16/256
"    u = u - 0.5;\n"
"    v = v - 0.5;\n"
"\n"
"    // YUV to RGB 转换矩阵 (BT.601)\n"
"    float r = y + 1.596 * v;\n"
"    float g = y - 0.392 * u - 0.813 * v;\n"
"    float b = y + 2.017 * u;\n"
"\n"
"    fragColor = vec4(r, g, b, 1.0);\n"
"}\n";


VideoWidget::VideoWidget(QWidget* parent)
    : QOpenGLWidget(parent), m_vbo(0), m_vao(0)
{
    QSurfaceFormat format;
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    setFormat(format);
}

VideoWidget::~VideoWidget()
{
    makeCurrent();
    if (m_textureY_id) glDeleteTextures(1, &m_textureY_id);
    if (m_textureU_id) glDeleteTextures(1, &m_textureU_id);
    if (m_textureV_id) glDeleteTextures(1, &m_textureV_id);
    if (m_vbo) glDeleteBuffers(1, &m_vbo);
    if (m_vao) glDeleteVertexArrays(1, &m_vao);
    doneCurrent();
}

void VideoWidget::initializeGL()
{
    initializeOpenGLFunctions();

    m_shaderProgram.bindAttributeLocation("vertexIn", 0);
    m_shaderProgram.bindAttributeLocation("textureIn", 1);

    m_shaderProgram.addShaderFromSourceCode(QOpenGLShader::Vertex, g_vertexShaderSource);
    m_shaderProgram.addShaderFromSourceCode(QOpenGLShader::Fragment, g_fragmentShaderSource);
    m_shaderProgram.link();

    m_shaderProgram.bind();
    m_shaderProgram.setUniformValue("tex_y", 0);
    m_shaderProgram.setUniformValue("tex_u", 1);
    m_shaderProgram.setUniformValue("tex_v", 2);
    m_shaderProgram.release();

    static const GLfloat vertices[] = {
        -1.0f, -1.0f,    0.0f, 1.0f,
         1.0f, -1.0f,    1.0f, 1.0f,
        -1.0f,  1.0f,    0.0f, 0.0f,
         1.0f,  1.0f,    1.0f, 0.0f,
    };

    glGenVertexArrays(1, &m_vao);
    glBindVertexArray(m_vao);
    glGenBuffers(1, &m_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), (void*)(2 * sizeof(GLfloat)));
    glEnableVertexAttribArray(1);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);

    // --- 【核心修改】 ---
    // 将纹理过滤器从 GL_LINEAR 改为 GL_NEAREST，以保留像素细节，避免模糊
    glGenTextures(1, &m_textureY_id);
    glBindTexture(GL_TEXTURE_2D, m_textureY_id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenTextures(1, &m_textureU_id);
    glBindTexture(GL_TEXTURE_2D, m_textureU_id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenTextures(1, &m_textureV_id);
    glBindTexture(GL_TEXTURE_2D, m_textureV_id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glBindTexture(GL_TEXTURE_2D, 0);

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
}

void VideoWidget::resizeGL(int w, int h)
{
    glViewport(0, 0, w, h);
    if (h > 0) {
        m_widget_aspect = static_cast<float>(w) / static_cast<float>(h);
    }
}

void VideoWidget::paintGL()
{
    glClear(GL_COLOR_BUFFER_BIT);

    if (m_video_w == 0 || m_video_h == 0) {
        return;
    }

    m_shaderProgram.bind();

    float scale_x = 1.0f;
    float scale_y = 1.0f;
    if (m_widget_aspect > m_video_aspect) {
        scale_x = m_video_aspect / m_widget_aspect;
        scale_y = 1.0f;
    }
    else {
        scale_x = 1.0f;
        scale_y = m_widget_aspect / m_video_aspect;
    }
    m_shaderProgram.setUniformValue("scale", scale_x, scale_y);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_textureY_id);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_textureU_id);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, m_textureV_id);

    glBindVertexArray(m_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);

    m_shaderProgram.release();
}

void VideoWidget::onFrameDecoded(AVFrame* frame)
{
    if (!frame || !frame->data[0] || !frame->data[1] || !frame->data[2] || frame->format != AV_PIX_FMT_YUV420P) {
        if (frame) av_frame_free(&frame);
        return;
    }

    makeCurrent();

    if (m_video_w != frame->width || m_video_h != frame->height) {
        m_video_w = frame->width;
        m_video_h = frame->height;
        if (m_video_h > 0) {
            m_video_aspect = static_cast<float>(m_video_w) / static_cast<float>(m_video_h);
        }

        glBindTexture(GL_TEXTURE_2D, m_textureY_id);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, m_video_w, m_video_h, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
        glBindTexture(GL_TEXTURE_2D, m_textureU_id);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, m_video_w / 2, m_video_h / 2, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
        glBindTexture(GL_TEXTURE_2D, m_textureV_id);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, m_video_w / 2, m_video_h / 2, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
    }

    glPixelStorei(GL_UNPACK_ROW_LENGTH, frame->linesize[0]);
    glBindTexture(GL_TEXTURE_2D, m_textureY_id);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, m_video_w, m_video_h, GL_RED, GL_UNSIGNED_BYTE, frame->data[0]);

    glPixelStorei(GL_UNPACK_ROW_LENGTH, frame->linesize[1]);
    glBindTexture(GL_TEXTURE_2D, m_textureU_id);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, m_video_w / 2, m_video_h / 2, GL_RED, GL_UNSIGNED_BYTE, frame->data[1]);

    glPixelStorei(GL_UNPACK_ROW_LENGTH, frame->linesize[2]);
    glBindTexture(GL_TEXTURE_2D, m_textureV_id);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, m_video_w / 2, m_video_h / 2, GL_RED, GL_UNSIGNED_BYTE, frame->data[2]);

    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

    doneCurrent();

    av_frame_free(&frame);

    update();
}