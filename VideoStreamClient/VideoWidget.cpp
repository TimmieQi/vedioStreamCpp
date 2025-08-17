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
// 【核心修改】顶点着色器现在接收一个缩放因子
static const char* g_vertexShaderSource =
"#version 330 core\n"
"layout (location = 0) in vec2 vertexIn;\n"
"layout (location = 1) in vec2 textureIn;\n"
"uniform vec2 scale;\n" // 新增 uniform 变量
"out vec2 textureOut;\n"
"void main()\n"
"{\n"
// 将顶点坐标与缩放因子相乘
"    gl_Position = vec4(vertexIn * scale, 0.0, 1.0);\n"
"    textureOut = textureIn;\n"
"}\n";

// 片段着色器保持不变
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
"    float u = texture(tex_u, textureOut).r - 0.5;\n"
"    float v = texture(tex_v, textureOut).r - 0.5;\n"
"    float r = y + 1.5748 * v;\n"
"    float g = y - 0.1873 * u - 0.4681 * v;\n"
"    float b = y + 1.8556 * u;\n"
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

    glGenTextures(1, &m_textureY_id);
    glBindTexture(GL_TEXTURE_2D, m_textureY_id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenTextures(1, &m_textureU_id);
    glBindTexture(GL_TEXTURE_2D, m_textureU_id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenTextures(1, &m_textureV_id);
    glBindTexture(GL_TEXTURE_2D, m_textureV_id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glBindTexture(GL_TEXTURE_2D, 0);

    // 【修改】使用黑色作为背景色，以实现黑边效果
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
}

void VideoWidget::resizeGL(int w, int h)
{
    glViewport(0, 0, w, h);
    // 【新增】当窗口大小改变时，更新控件的宽高比
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

    // 【核心新增】计算缩放因子并上传
    float scale_x = 1.0f;
    float scale_y = 1.0f;
    if (m_widget_aspect > m_video_aspect) {
        // 控件比视频宽 -> 垂直填满，水平按比例缩放 (上下黑边)
        scale_x = m_video_aspect / m_widget_aspect;
        scale_y = 1.0f;
    }
    else {
        // 控件比视频高 -> 水平填满，垂直按比例缩放 (左右黑边)
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
        // 【新增】当视频分辨率确定时，计算视频的宽高比
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