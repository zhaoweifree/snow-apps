# 贡献指南 / Contributing

## 中文

感谢你关注 Snow Apps！提交贡献前，请先阅读以下规则。

### Pull Request 范围

- 本项目不直接接受新增功能或功能增强的 PR，仅审查修复问题的 PR。
- 如果你需要某项功能，请先在社区发起讨论，说明使用场景、遇到的问题和期望的行为。
- 发起功能讨论并不意味着相关功能 PR 会被接受；请先通过讨论明确后续安排。

### 提交修复 PR

- 说明问题、复现步骤、预期行为，以及修复方案；如有相关 Issue，请附上链接。
- 保持改动聚焦于所修复的问题，避免夹带新功能或无关改动。
- 遵循 [AGENTS.md](AGENTS.md) 中的开发规范，为行为变更添加确定性、可离屏运行的测试。
- 仅运行与改动相关的测试，并在 PR 中说明受影响的构建预设或目标，以及格式检查、静态检查和相关测试的结果。

## English

Thank you for your interest in Snow Apps! Please read these guidelines before contributing.

### Pull request scope

- This project does not directly accept PRs that add or enhance features. Only fix PRs will be reviewed.
- If you need a specific feature, first start a community discussion describing your use case, the problem, and the expected behavior.
- Starting a feature discussion does not imply that a feature PR will be accepted; use the discussion to agree on next steps first.

### Submitting a fix PR

- Describe the problem, reproduction steps, expected behavior, and proposed fix. Link any related issues.
- Keep changes focused on the problem being fixed, without adding features or unrelated changes.
- Follow the development guidelines in [AGENTS.md](AGENTS.md) and cover behavioral changes with deterministic tests that can run offscreen.
- Run only the tests related to your changes. In the PR, list the affected build presets or targets and report formatting, lint, and related test results.
