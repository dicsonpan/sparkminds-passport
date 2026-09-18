# SparkMinds 学员设备发卡花名册 (Student Provision Roster)

以下为已在官方后台注册并分配的学员硬件设备密钥及 NFC 信息表：

| 姓名 | 学号 (Passport ID) | 32 字节硬件密钥 (Secret Hex) | NFC Token | NFC URL |
| :--- | :--- | :--- | :--- | :--- |
| **梁根润** | `SM-2026-001` | `8900f114af58cb2989c7856e0c93a49d39ad1f8e2f756d682540a7d6266ee37d` | `4650107eeb0f69aa` | https://api.sparkminds.io/n/4650107eeb0f69aa |
| **陆昭闻（Owen）** | `SM-2026-002` | `9d6a4ea3a3f9802a40969b4676c65b73035e6cdcc1c4b970661c4f61b79c2819` | `639df345543681ff` | https://api.sparkminds.io/n/639df345543681ff |
| **仇绍恒** | `SM-2026-003` | `32fa28d5be83704fd8eb77d19b6b8f9f1061fa77449f20d1d56a381ecea5aade` | `c4b063d14f537f78` | https://api.sparkminds.io/n/c4b063d14f537f78 |
| **梁根珹** | `SM-2026-004` | `913240fb10d0009522253bb392764b6ec1453c57fd328cd178ae7bc3a7eb9e65` | `3472528c0ff9629e` | https://api.sparkminds.io/n/3472528c0ff9629e |
| **王之谦** | `SM-2026-006` | `d3df7c02ca477729de047b2c576a4b03274c0c80957bce3599e58e51a5d1841b` | `ae3773960e3c0738` | https://api.sparkminds.io/n/ae3773960e3c0738 |
| **徐一潇（Jeremy）** | `SM-2026-013` | `155e787acf59d130ce453fe22a498281098f0abce690d7049f0f8ee6c0c2aab1` | `25b7591a5eeb96f0` | https://api.sparkminds.io/n/25b7591a5eeb96f0 |
| **王宥森** | `SM-2026-051` | `2f2c7908e132801fb4bab69398c8d39e37192aa24af5a3a819c2e653d1a4ebed` | - | - |
| **郭涵若 Cavin** | `SM-2026-070` | `4a043814904f8e31668012d1997f5d2ba313ad1f39a7b021553341b266b9678c` | `5841c1a98ffae479` | https://api.sparkminds.io/n/5841c1a98ffae479 |
| **Aiden Kwok** | `SM-2026-193` | `93fc265de7a4f6bb21d6f24dc65a502d861d2bb4d5dd1c3b979064bcd702f747` | `9fb92b6a0bfcf847` | https://api.sparkminds.io/n/9fb92b6a0bfcf847 |
| **Eric 金家熠** | `SM-2026-565` | `4cc41779c7a70d6475de87bb0f1267f5dfbceede55e39c137ae36c9b6aac5e3d` | `e2167077254cf8a3` | https://api.sparkminds.io/n/e2167077254cf8a3 |

---

### 快速切换/烧录指令
固件已内置花名册解析引擎，只需在 ADB Shell 或串口控制台中输入：
```bash
provision 001   # 一键切换为 梁根润
provision 002   # 一键切换为 陆昭闻 Owen
provision 003   # 一键切换为 仇绍恒
provision 004   # 一键切换为 梁根珹
provision 006   # 一键切换为 王之谦
provision 013   # 一键切换为 徐一潇
provision 051   # 一键切换为 王宥森
provision 070   # 一键切换为 郭涵若 Cavin
provision 193   # 一键切换为 Aiden Kwok
provision 565   # 一键切换为 Eric 金家熠
```
系统将自动更新卡片中的 `pid`、`secret`、`name`、`student_id` 并安全清空上一位学员的历史缓存。
