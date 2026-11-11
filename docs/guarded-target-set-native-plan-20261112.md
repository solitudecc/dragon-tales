# guarded-target-sets 独立原生验收预注册

本组独立派生于已冻结的业务循环模型，旧 C/D 源、输入与结果不改。实际调用公开 `PassPipeline.add("guarded-target-sets")`，新独立 metadata 仅预测候选；不把普通 `dragon.guarded-entry` 的 memory 改为候选提示。

五模式为普通循环、实际 LOAD 前写表、LOAD 后写表、目标块外未知调用、目标块内未知调用。每模式覆盖 x=0/1/2^63/UINT64_MAX、循环上界0..3、八个目标值43/44/99/127/128/255/777/99999、正常65536与不足511的 Guest size，共1280输入。前六个为正常候选集；777是原 switch 的 central case；99999走原default。另以合法过时提示集合{777}作正向语义兼容，不把提示未命中当错误。

两个候选集合分别生成输出；普通/写表/块外调用共四模式应实际复制 terminal block，块内未知调用应保持原IR。before、六候选输出、过时提示输出在 O0/O2 共7680完整状态比较/构建，其中before2560，变换后4096，未改写的调用模式1024。包含3840边界失败路径，不能称全部根正常返回；同一1280输入跨优化级和构建重复不算新增样本。

完整比较返回值、调用次数和73728字节arena（包括512字节寄存器、64KiB Guest、七个 Exit 字段、其余缓冲和描述符），不归一化、不排除。每次正常比较对返回值、调用次数、寄存器、Guest与七个Exit字段各破坏一位，总84480比较器控制/构建。五故障IR分别为无条件信任43、43错跳另一case、LOAD后写表再错误重读、删去LOAD前STORE、重复case/default PHI值加一。每构建2560故障观察；应检测数由冻结独立参考实际差异决定，原始流完整保留，非差异输入不伪计检测。

结构门槛：actual LOAD只执行一次，six-case small switch与原完整7-case/default fallback共享该SSA；default与两个case同目标的PHI保留3条原边+3条fallback+2条fast边。过时集合{777}保留6条good PHI边。未知调用不被复制。无manifest、坏JSON、非case提示、17值超限与1block预算均保持不变。Module/Function/Arguments/global身份与原entry metadata及独立sentinel必须保留。

原单候选core/auto ×43/99×六fixture共24输出用相同输入路径与旧driver和旧D运行库生成并冻结，新E须逐字节相同。此项是字节兼容，不重计以前native证据。主构建与noZ3分别运行、分别审计完整40条压缩流/10240帧，保存所有退出码、失败、源和运行库前后SHA。

范围仅验证新小目标集实际守卫及完整fallback语义，不证明整程序去平坦化，也不把原始文件表字节视作Guest不变事实。真实四ELF及完整循环闭合由另一组独立验收。
