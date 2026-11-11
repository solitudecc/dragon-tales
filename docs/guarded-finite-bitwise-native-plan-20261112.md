# G 有限bitwise候选独立验收预注册

新增单独TestLLVMGuardedFiniteBitwise.cpp（360f6e...）在旧F builtin存在的两运行库上实际4cases/77assert，3个positive cases各因changed=false失败，negative case通过。旧失败是精度缺口，不是API不存在、非法IR或错误原生语义。生产仅扩候选计算，原flags/实际LOAD/fullfallback仍执行。

新8模式独立派生，不改F封存模型：unknown i32&1→or disjoint24→zext nneg；PHI{0,50,unknown}与select{0,50}分别经shl3 OR disjoint base；unknown&3→xor1→or24；LOAD前/后改表；sparse64高位掩码0x8000000000000001；dense15拒绝。native用base512保持Guest64KiB可执行，实际90120/73792由单测两种PHI/select及真实样本覆盖。普通每mode x四边值×loop上界0..3×target八值×size正常/不足，共2048输入。sparse高位输入必须走原state-size失败出口，不能把高位掩码当静态跳转界。

7模式要求真实改写，combo_phi复制left重复2slot与right1slot，unknown第三边保持；其它正例各1copy，dense完全不改。预测目标集合、same-LOAD完整fallback、flags、Module/Function/Argument/global/metadata身份都检查；bad manifest、zero rewrite、1block预算不改。mask0/full/未知mask/8值上限/i128/精确有限AndOrXor/直接poison等边界由新4case单测覆盖，不伪称全部做了native。

每构建before/after × O0/O2共8192正常观察，before4096、真正变换after3584、dense未改after512；3840正常业务返回、4352原边界失败。7故障IR（信任hint、错case、重读LOAD、丢STORE、错PHI、fresh代old、错选state）共3584观察，应错数由独立模型判定。正常比较使用返回值、callcount与完整73728字节arena；90112比较器控制，无跳过/归一化。46完整gzip流/11776帧重新全流审计。相同2048输入跨优化级和构建重复不当作新样本。

主/noZ3各执行一次，同源同输入。F旧69份已保存输出可做相同绝对输入路径的字节兼容，若一致不重跑或重计F原生。CALL保守行为的旧F模型因此保留，G新native不新增CALL场景。所有旧精度失败与新失败均保留，不改变模型追绿。
