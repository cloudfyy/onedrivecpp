# 文档中心

[English](README.md) | 简体中文

文档按使用任务划分。新用户建议按章节顺序阅读；运维人员和开发者可以直接进入
对应的参考章节。

## 第一章：开始使用

1. [项目介绍、编译与安装](../README.zh-CN.md)
2. [Microsoft 认证](authentication.zh-CN.md)
3. [配置](configuration.zh-CN.md)
4. [使用与命令输出](usage.zh-CN.md)

## 第二章：同步与运维

1. [同步与恢复](synchronization.zh-CN.md)
2. [配置参考](configuration.zh-CN.md)
3. [运行诊断与只读命令](usage.zh-CN.md)

同步章节说明 Delta 处理、冲突保护、原子传输、移动与删除恢复、数据库修复和
运行故障处理。配置章节是选择规则、同步模式、传输限制、Monitor、代理、权限及
存储布局的权威参考。

## 第三章：设计与开发

1. [架构](architecture.zh-CN.md)
2. [开发与测试](development.zh-CN.md)
3. [许可证、法律与品牌](project.zh-CN.md)

这些章节说明依赖边界、事务状态机、源码目录职责、真实 Graph 测试、系统边界
测试和项目采用的 C++ Core Guidelines 检查。

## 快速链接

- 编译与安装：[项目 README](../README.zh-CN.md#编译)
- 首次授权：[Microsoft 认证](authentication.zh-CN.md#授权客户端)
- 完整 TOML 示例：[配置](configuration.zh-CN.md#配置参考示例)
- 选择性同步：[配置](configuration.zh-CN.md#选择与过滤)
- 冲突与删除策略：[配置](configuration.zh-CN.md#冲突处理)
- 状态重置与数据库修复：
  [同步与恢复](synchronization.zh-CN.md#状态重置与-delta-重建)
- 故障诊断输出：[使用与命令输出](usage.zh-CN.md)
- 开发检查：[开发与测试](development.zh-CN.md#c-core-guidelines-检查)
