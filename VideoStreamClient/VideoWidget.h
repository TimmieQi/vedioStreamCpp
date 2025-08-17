#pragma once

#include <QOpenGLWidget>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLShaderProgram>
#include <QOpenGLTexture>
#include <memory>
#include <vector>

struct AVFrame;

class VideoWidget : public QOpenGLWidget, protected QOpenGLFunctions_3_3_Core
{
    Q_OBJECT

public:
    explicit VideoWidget(QWidget* parent = nullptr);
    ~VideoWidget();

public slots:
    void onFrameDecoded(AVFrame* frame);

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;

private:
    QOpenGLShaderProgram m_shaderProgram;

    GLuint m_textureY_id = 0;
    GLuint m_textureU_id = 0;
    GLuint m_textureV_id = 0;

    int m_video_w = 0;
    int m_video_h = 0;

    // 【新增】用于存储宽高比的成员变量
    float m_video_aspect = 1.0f;
    float m_widget_aspect = 1.0f;

    GLuint m_vbo;
    GLuint m_vao;
};