# 贡献指南 / Contributing Guide

[English](#english) | 简体中文

---

## 简体中文

感谢您对本项目的关注！欢迎提交 Issue 和 Pull Request。

### 如何贡献

1. **Fork** 本仓库
2. 创建功能分支：`git checkout -b feature/my-feature`
3. 提交更改：`git commit -m 'Add my feature'`
4. 推送分支：`git push origin feature/my-feature`
5. 提交 **Pull Request**

### 开发规范

- 遵循现有代码风格（C++17）
- 添加必要的测试
- 更新相关文档
- 确保所有契约测试通过

### 测试

```bash
cd native-switch/tests
python test_*.py
```

所有 11 个契约测试必须通过。

### 提交信息格式

```
<type>: <简短描述>

<详细说明（可选）>
```

类型：`feat`、`fix`、`docs`、`test`、`refactor`、`chore`

---

## English

Thank you for your interest! Issues and Pull Requests are welcome.

### How to Contribute

1. **Fork** this repository
2. Create a feature branch: `git checkout -b feature/my-feature`
3. Commit changes: `git commit -m 'Add my feature'`
4. Push branch: `git push origin feature/my-feature`
5. Submit a **Pull Request**

### Development Guidelines

- Follow existing code style (C++17)
- Add necessary tests
- Update relevant documentation
- Ensure all contract tests pass

### Testing

```bash
cd native-switch/tests
python test_*.py
```

All 11 contract tests must pass.

### Commit Message Format

```
<type>: <short description>

<detailed description (optional)>
```

Types: `feat`, `fix`, `docs`, `test`, `refactor`, `chore`
