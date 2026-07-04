#version 430 core

// 【线程组配置】
// 定义每个工作组（Work Group）内的本地线程大小。
// 这里设置单排 10 个线程（X=10, Y=1, Z=1），完美对应我们要渲染的 10 个双摆。
layout(local_size_x = 64, local_size_y = 1) in;

// 【双摆状态结构体】
// 该结构体的大小和排列顺序必须与 C++ 端（main.cpp）的 struct 严格 1:1 对齐。
struct PendulumState {
    float t1, t2;     // 摆1和摆2的当前角度 (弧度)
    float td1, td2;   // 摆1和摆2的角速度 (theta dot)
    float l1, l2;     // 摆1和摆2的摆长
    float m1, m2;     // 摆1和摆2的质点质量
};

// 【SSBO 显存共享缓冲区】
// layout(std430) 指定标准的内存对齐方式；binding = 1 对应 C++ 中 glBindBufferBase 的绑定点。
// 整个数组存放在显卡的高速显存中，GPU 线程可以直接对其进行读写，从而实现无内存拷贝的物理更新。
layout(std430, binding = 1) buffer PendulumBuffer {
    PendulumState pendulums[]; // 动态大小的角度与状态数组
};

// 【外部统一变量】
uniform float u_dt;         // 从 C++ 传进来的上一帧渲染时间差（秒），用于控制物理模拟的时钟
uniform int u_total_count;   // 总双摆数量（当前为 10）

// 【物理常数】
const float g = 9.81;        // 重力加速度
const float step_size = 0.000005; // 固定的微小物理步长。步长越小，多步累加的欧拉积分越精确，能量越守恒

void main() {
    // 获取当前 GPU 线程在全局空间中的唯一索引 ID
    int idx = int(gl_GlobalInvocationID.x);
    
    // 边界安全检查，防止线程越界访问未定义的显存
    if (idx >= u_total_count) return;

    // 从 SSBO 显存中拷贝当前双摆的数据到显卡局部寄存器（高速缓存）
    PendulumState p = pendulums[idx];

    // 【子步迭代计算（Sub-stepping）】
    // 为了防止画面掉帧时 delta_t 过大导致物理模拟崩溃（爆飞），
    // 我们将一帧的时间拆解为多步极小的 step_size 进行高频迭代。
    int steps = int(ceil(u_dt / step_size));
    if (steps > 150) steps = 150; // 设置硬上限，防止极限掉帧时显卡陷入死循环

    // 物理引擎核心循环（执行拉格朗日力学离散积分）
    for (int i = 0; i < steps; ++i) {
        float delta_t = (p.t1 - p.t2); // 两节摆的角度差值
        
        // 分母项：双摆运动方程中共享的运动分母，若趋近于0会导致除零崩溃，做极小值平滑
        float bottom = (2.0 * p.m1) + p.m2 - (p.m2 * cos(2.0 * delta_t));
        if (abs(bottom) < 1e-5) bottom = 1e-5;

        // 分子项1：计算摆1的角加速度分子
        float num1 = -g * (2.0 * p.m1 + p.m2) * sin(p.t1)
            - p.m2 * g * sin(p.t1 - 2 * p.t2)
            - 2 * sin(delta_t) * p.m2 * (p.td2 * p.td2 * p.l2 + p.td1 * p.td1 * p.l1 * cos(delta_t));

        // 分子项2：计算摆2的角加速度分子
        float num2 = 2 * sin(delta_t) * (p.td1 * p.td1 * p.l1 * (p.m1 + p.m2)
            + g * (p.m1 + p.m2) * cos(p.t1)
            + p.td2 * p.td2 * p.l2 * p.m2 * cos(delta_t));

        // 获得当前时刻的角加速度（theta_dd1 和 theta_dd2）
        float theta_dd1 = num1 / (p.l1 * bottom);
        float theta_dd2 = num2 / (p.l2 * bottom);

        // 半隐式欧拉积分（Semi-implicit Euler Integration）
        p.td1 += step_size * theta_dd1; // 1. 先用加速度更新角速度
        p.td2 += step_size * theta_dd2;
        p.t1 += step_size * p.td1;     // 2. 再用更新后的角速度更新角度
        p.t2 += step_size * p.td2;
    }

    // 【写回显存】将寄存器中计算完毕的最新物理状态，重新覆盖写入显存 SSBO
    pendulums[idx] = p;
}