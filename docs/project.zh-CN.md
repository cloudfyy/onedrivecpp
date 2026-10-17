# 许可证、法律与品牌

[English](project.md) | 简体中文 |
[文档中心](README.zh-CN.md)

## 许可证

项目采用 GPL-3.0-or-later。参考项目同样使用 GPLv3，因此，将来可以在遵守
许可证要求的前提下复用或改写其设计。

完整 GPLv3 文本和项目“或任何后续版本”的声明见 [LICENSE](../LICENSE)。
CMake 安装和 DEB 包均将 `LICENSE` 与机器可读的 `copyright` 文件安装到
文档目录，通常为 `<prefix>/share/doc/onedrive-cpp`，标准 DEB 包使用
`/usr/share/doc/onedrive-cpp`。使用随包 FTXUI/Proxy 源码时，
还会将其原始许可证安装到 `licenses` 子目录。

## 项目与包元数据

本项目源码位于 [cloudfyy/onedrivecpp](https://github.com/cloudfyy/onedrivecpp)。
`debian/control` 统一定义包维护者、项目主页和描述，CPack 读取相同字段，
不再单独维护一份。上文提到的参考项目并不是本 C++ 实现的源码来源。

## 法律与品牌材料

- [服务条款](../TERMS.md)
- [隐私声明](../PRIVACY.md)
- 项目 Logo：[SVG](../assets/onedrive-cpp-logo.svg) |
  [512x512 PNG](../assets/onedrive-cpp-logo.png)

这些法律文件是项目维护者拟定的草案，不构成法律建议。正式公开或商业发布前，
应根据实际运营主体和适用司法管辖区进行审核。onedrive-cpp 是独立项目，与
Microsoft 不存在隶属关系，也未获得其背书。
