#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <iostream>
#include <vector>
#include <cmath>
#include <string>
#include <fstream>
#include <sstream>

// 【全局渲染配置】
const unsigned int SCR_WIDTH = 1000;  // 窗口宽度
const unsigned int SCR_HEIGHT = 1000; // 窗口高度
const int COUNT = 200;                // 并行模拟及可视化的双摆数量

// 【C++ 数据结构对齐】
// 必须与显卡 GLSL 中的 struct 保持绝对相同的成员变量类型与顺序
struct PendulumState
{
  float t1, t2;     // 摆角
  float td1, td2;   // 角速度
  float l1, l2;     // 摆长
  float m1, m2;     // 质量
};

// 【辅助函数：动态编译 Compute Shader】
unsigned int compileComputeShader(const char* path)
{
  std::string code; std::ifstream file(path); std::stringstream ss;
  if (file) { ss << file.rdbuf(); code = ss.str(); }
  const char* src = code.c_str();
  unsigned int shader = glCreateShader(GL_COMPUTE_SHADER);
  glShaderSource(shader, 1, &src, NULL); glCompileShader(shader);

  // 错误检查逻辑
  int success; char infoLog[512]; glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
  if (!success) { glGetShaderInfoLog(shader, 512, NULL, infoLog); std::cerr << "GPU代码编译错误:\n" << infoLog << "\n"; }

  unsigned int program = glCreateProgram(); glAttachShader(program, shader); glLinkProgram(program);
  return program;
}

int main()
{
  // ---------------------------------------------------------------------
  // 1. 初始化 OpenGL 上下文环境
  // ---------------------------------------------------------------------
  glfwInit();
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4); // 开启 Compute Shader 必须要求最低 OpenGL 4.3
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  GLFWwindow* window = glfwCreateWindow(SCR_WIDTH, SCR_HEIGHT, "10 Double Pendulums Visualizer", NULL, NULL);
  glfwMakeContextCurrent(window);
  gladLoadGLLoader((GLADloadproc)glfwGetProcAddress);
  // glfwSwapInterval(1);

  // ---------------------------------------------------------------------
  // 2. 初始化 CPU 端的物理状态，并一次性打包上传到 GPU (SSBO)
  // ---------------------------------------------------------------------
  std::vector<PendulumState> init_data(COUNT);
  float base_theta1 = 1.57079f; // 主初始角度：90度 (水平放开)
  float base_theta2 = 1.57079f;

  for (int i = 0; i < COUNT; ++i)
  {
    init_data[i].t1 = base_theta1;
    // 【混沌系统扰动】
    // 每一个双摆的第二节摆角在初始时增加 0.0001 弧度的微小扰动
    init_data[i].t2 = base_theta2 + (i * 0.0001f);
    init_data[i].td1 = init_data[i].td2 = 0.0f;

    // 摆长设置
    init_data[i].l1 = 0.5f;
    init_data[i].l2 = 0.5f;

    // 【恢复原生力道】质量恢复为 1:1 均匀对等状态
    init_data[i].m1 = 1.0f;
    init_data[i].m2 = 1.0f;
  }

  // 生成并配置 Shader Storage Buffer Object (SSBO)
  unsigned int ssbo;
  glGenBuffers(1, &ssbo);
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo);
  // 上传初始结构体数组到显卡
  glBufferData(GL_SHADER_STORAGE_BUFFER, init_data.size() * sizeof(PendulumState), init_data.data(), GL_DYNAMIC_COPY);
  // 绑定到插槽 1，与 GLSL 中的 layout(binding = 1) 连通
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, ssbo);

  // ---------------------------------------------------------------------
  // 3. 编译 GPU 物理与 2D 线框传统的渲染着色器
  // ---------------------------------------------------------------------
  unsigned int computeProgram = compileComputeShader("pendulum_compute.glsl");

  // 用于绘制单色线段的极简渲染管线（直接使用顶点传入坐标，输出 uniform 颜色）
  const char* vs = "#version 330 core\nlayout(location=0) in vec2 aPos; void main(){ gl_Position=vec4(aPos,0.0,1.0); }";
  const char* fs = "#version 330 core\nout vec4 FragColor; uniform vec3 uColor; void main(){ FragColor=vec4(uColor, 1.0); }";
  unsigned int vShader = glCreateShader(GL_VERTEX_SHADER); glShaderSource(vShader, 1, &vs, NULL); glCompileShader(vShader);
  unsigned int fShader = glCreateShader(GL_FRAGMENT_SHADER); glShaderSource(fShader, 1, &fs, NULL); glCompileShader(fShader);
  unsigned int lineProgram = glCreateProgram(); glAttachShader(lineProgram, vShader); glAttachShader(lineProgram, fShader); glLinkProgram(lineProgram);

  // 【配置几何绘制缓冲区】
  // 每个双摆包含2条连续线段，每条线2个端点，每个端点2个float坐标 (x,y)。
  unsigned int vao, vbo;
  glGenVertexArrays(1, &vao);
  glGenBuffers(1, &vbo);
  glBindVertexArray(vao);
  glBindBuffer(GL_ARRAY_BUFFER, vbo);
  glBufferData(GL_ARRAY_BUFFER, COUNT * 4 * 2 * sizeof(float), NULL, GL_DYNAMIC_DRAW); // 开辟动态写缓冲区
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);

  // ---------------------------------------------------------------------
  // 4. 数据缓存开辟与绘制预设
  // ---------------------------------------------------------------------
  float lastFrame = (float)glfwGetTime();
  std::vector<PendulumState> current_states(COUNT); // 用于承接从 GPU 倒回来的角度数据
  std::vector<float> line_vertices(COUNT * 4 * 2);  // 用于存放转化完成的二维屏幕几何坐标
  glLineWidth(2.5f);                                // 设置线条渲染宽度

  // ---------------------------------------------------------------------
  // 5. 渲染与计算主循环
  // ---------------------------------------------------------------------
  while (!glfwWindowShouldClose(window))
  {
    // 计算稳定的时钟帧差 DeltaTime
    float currentFrame = (float)glfwGetTime();
    float deltaTime = currentFrame - lastFrame;
    lastFrame = currentFrame;
    if (deltaTime > 0.1f) deltaTime = 0.016f; // 防止窗口拖动造成的超大延迟时间导致模拟爆炸

    // 【STEP 1: GPU 物理计算】
    glUseProgram(computeProgram);
    // 【恢复原生力道】去除时间放大系数，直接使用纯粹的真实帧差 deltaTime
    glUniform1f(glGetUniformLocation(computeProgram, "u_dt"), deltaTime);
    glUniform1i(glGetUniformLocation(computeProgram, "u_total_count"), COUNT);
    // glDispatchCompute(1, 1, 1); // 启动 1 个工作组（里面有 10 个物理线程）
    glDispatchCompute((COUNT + 63) / 64, 1, 1);

    // 【显卡内存同步栅栏】保证下方的 C++ 读取动作在 GPU 彻底算完之后才发生
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

    // 【STEP 2: 回读物理状态】将显卡 SSBO 中算好的最新角度倒回内存 current_states 中
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, COUNT * sizeof(PendulumState), current_states.data());

    // 【STEP 3: 几何投影转换 (正向运动学)】
    float x_origin = 0.0f; // 10个双摆统一绑定的悬挂点：屏幕正中心
    float y_origin = 0.2f;

    for (int i = 0; i < COUNT; ++i)
    {
      auto& p = current_states[i];

      // 利用极坐标三角函数算出：摆1端点(x1,y1) 和 摆2端点(x2,y2)
      float x1 = x_origin + p.l1 * std::sin(p.t1);
      float y1 = y_origin - p.l1 * std::cos(p.t1);
      float x2 = x1 + p.l2 * std::sin(p.t2);
      float y2 = y1 - p.l2 * std::cos(p.t2);

      size_t v_idx = i * 8; // 算出当前摆在顶点数组中的内存起始偏置
      // 第一条线：悬挂中心点 -> 第一节关节 (x1, y1)
      line_vertices[v_idx + 0] = x_origin; line_vertices[v_idx + 1] = y_origin;
      line_vertices[v_idx + 2] = x1;       line_vertices[v_idx + 3] = y1;
      // 第二条线：第一节关节 (x1, y1) -> 第二节末端质点 (x2, y2)
      line_vertices[v_idx + 4] = x1;       line_vertices[v_idx + 5] = y1;
      line_vertices[v_idx + 6] = x2;       line_vertices[v_idx + 7] = y2;
    }

    // 【STEP 4: 几何重绘与上色】
    glClearColor(0.05f, 0.05f, 0.05f, 1.0f); // 深灰色背景
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(lineProgram);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    // 将 CPU 计算好的崭新多骨架端点坐标一次性推入显卡顶点动态缓冲区
    glBufferSubData(GL_ARRAY_BUFFER, 0, line_vertices.size() * sizeof(float), line_vertices.data());

    // 独立为 10 个双摆进行不同的色彩循环并绘制
    for (int i = 0; i < COUNT; ++i)
    {
      // 依据双摆的索引产生渐变彩虹色，便于肉眼在后期彻底散开时辨别个体
      float r = 0.2f + 0.8f * ((float)i / COUNT);
      float g = 0.5f + 0.5f * std::sin(i);
      float b = 1.0f - 0.8f * ((float)i / COUNT);
      glUniform3f(glGetUniformLocation(lineProgram, "uColor"), r, g, b);

      // 每 4 个顶点绘制一个完整的双摆线框 (包含两条 GL_LINES)
      glDrawArrays(GL_LINES, i * 4, 4);
    }

    glfwSwapBuffers(window);
    glfwPollEvents();
  }

  // 清理管线与显存资源
  glDeleteVertexArrays(1, &vao);
  glDeleteBuffers(1, &vbo);
  glDeleteBuffers(1, &ssbo);
  glfwTerminate();
  return 0;
}