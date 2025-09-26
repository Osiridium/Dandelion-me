注意，你的任务是严格按要求完成本项目的代码，完成代码后不需要测试，但是需要用中文给我完整的解释一下你修改的内容。你只需要完成我发给你的实验要求里面让你完成的部分代码，尽量不要修改其他无关代码（除非你认为它对于完成功能来说是绝对必要的，或者我有额外的指示说明）。

实验要求的格式例子如下：
### 1.2.6 要求

按照整个软光栅化渲染器的渲染流程, 请对以下提到的文件中的函数空缺处进行填写, 使软光栅化渲染器能得到正确的结果:

* `rasterizer_renderer.cpp`
  * `RasterizerRenderer::render`: 遍历场景, 设置 Uniforms 变量并将顶点数据打包成 VertexShaderPayload 送入队列。
  * `VertexProcessor::worker_thread`: 从顶点输入队列中取出顶点数据, 调用 vertex shader 进行坐标变换。
  * `FragmentProcessor::worker_thread`: 从片元队列中取出片元数据, 调用 fragment shader 完成着色。
* `rasterizer.cpp`:
  * `Rasterizer::worker_thread`: 从顶点输出队列中取出数据, 组装图元 (三角形) 并光栅化之。
  * `Rasterizer::inside_triangle`: 给定像素坐标 (x, y) 以及三角形的三个顶点坐标, 判断 (x, y)

---
是否在三角形的内部。
  * `Rasterizer::compute_barycentric_2d`: 给定像素坐标 (x, y) 以及三角形的三个顶点坐标, 计算 (x, y) 对应的重心坐标 (α, β, γ)。
  * `Rasterizer::rasterize_triangle`: 对当前三角形进行光栅化, 在这部分中, 深度值, 顶点坐标和法线均进行了透视矫正插值。
* `shader.cpp`:
  * `vertex_shader`: 将顶点坐标变换到投影平面, 再进行视口变换; 同时将法线向量变换到相机坐标系用于后续插值。
  * `phong_fragment_shader`: 使用 Blinn Phong 模型计算每个片元 (像素) 的颜色。
 
  对于这个实验，你就只能修改rasterizer_renderer.cpp，rasterizer.cpp，shader.cpp里提及的函数。
