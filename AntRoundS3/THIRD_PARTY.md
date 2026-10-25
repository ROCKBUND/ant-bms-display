# 第三方来源

- LVGL **8.3.11**，官方发布标签 `v8.3.11`：https://github.com/lvgl/lvgl/tree/v8.3.11 。本工程分发原 `src/`、`lvgl.h`、`LICENCE.txt`，位于 `src/lvgl/`。源码 ZIP 的 SHA-256：`a992bdb8a13b9b706857889ac88d28e76d23c0aa57e1da8ddfc2816a093171c1`。MIT 许可保存在 `src/lvgl/LICENCE.txt`。
- 在上游源码目录中仅添加 `src/lvgl/src/lv_conf.h` 转发头，固定使用工程的 `src/lv_conf.h`，避免其他已安装 LVGL 的配置影响本工程。上游文件内容保持原样；应用配置由官方配置模板修改而来。内置字体仅启用所需尺寸，禁用未使用的控件、主题、文件系统与图片解码器。
- Montserrat 字体：LVGL 内置生成数据，源文件头记录了 Montserrat-Medium 和字号参数。SIL Open Font License 1.1，完整许可见 `licenses/Montserrat-OFL.txt`；来源 https://github.com/JulietaUla/Montserrat 。
- Font Awesome 5：LVGL 的内置字体数据同时包含其符号。许可文本来自官方 5.15.4 标签，保存在 `licenses/FontAwesome-LICENSE.txt`；来源 https://github.com/FortAwesome/Font-Awesome/tree/5.15.4 。
- GFX Library for Arduino **1.6.9**，https://github.com/moononournation/Arduino_GFX 。沿用外部 Arduino 库，不在此工程内重复分发。
- NimBLE-Arduino **2.5.0**，https://github.com/h2zero/NimBLE-Arduino 。沿用外部 Arduino 库，不在此工程内重复分发。
- 原 BMS 协议移植来源：用户提供的 csl2007 / BMS_Screen 源码，原始参考资料未包含在本仓库中。本次未改动协议和蓝牙任务。

UI 为本工程编写的 LVGL 绘图代码，不包含开源手表固件、在线示例工程或 AI 预览图。AI 预览图仅用于确认外观方向。
