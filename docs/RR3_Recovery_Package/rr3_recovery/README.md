# RR3 恢复规划包

这是对 `PROJECT_STATUS.md`（2026-09-09）的审阅与重规划，不包含修改过的 NRO、游戏数据或已经验证的运行补丁。

先看 `RR3_RECOVERY_PLAN.md`。让本地 Codex 从 `CODEX_START_HERE.md` 开始执行，并读取实际仓库文件。

`audit_rr3_status.py` 需要 Python 3.10 或更新版本，仅用标准库。它只读取报告；提供 `--elf` 时可核对 SHA-256 和 ELF64 AArch64 PT_LOAD 范围，不执行、解密或改写游戏二进制。

```powershell
python audit_rr3_status.py "你的PROJECT_STATUS.md完整路径" --out status_audit.json
python -m unittest discover -s . -p "test_audit_rr3_status.py" -v
```

退出码：0 表示请求的静态检查通过；1 表示发现审计错误；2 表示输入或工具错误。原报告含 hideSplash delta 错误，因此本次产生的 `RR3_STATUS_AUDIT.json` 为 FAIL，这是预期的审计结果。

`AUDITOR_TEST_RESULTS.txt` 是附件审计脚本自身的运行测试结果，不是 RR3 项目测试，更不是真机测试。

仅有报告时，无法证明具体构造器、GOT relocation、JNI 方法签名、回调参数或 NRO 运行行为。规划明确列出这些待验证内容，不要把计划中的候选链条当成已恢复的源代码。
