#!/bin/bash
# T16/M2 验收入口（薄壳）——主体在 scripts/gate/verify.sh（仓库脚本分层
# 约定见 scripts/README.md：门禁脚本进 gate/，入口从仓库根调用）。
# 此处存在仅为满足 M2 criteria 字面命令 `sudo ./verify.sh`（task.json
# milestones.M2）。
exec bash "$(dirname "$(readlink -f "$0")")/scripts/gate/verify.sh" "$@"
