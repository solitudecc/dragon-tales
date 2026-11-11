# H 独立STORE模型预注册

八模式为plain、无关最后STORE提示、A被后写、B被后写、table LOAD前写、table LOAD后写、OR地址、CALL barrier。全新独立顺序byte-array模型，不修改E/F/G封存源。初始unknown入口A=x&63不能供有限STORE提示；循环seed真实select8/38写A，两个重复successor slot进入同一head。复制路径有fresh caller-side LOAD/freeze、LOAD A→STORE B→LOAD B→targetLOAD，旧fresh/target进入head PHI，所有sideexit和actual LOAD/fullfallback保留。

初始state STORE由size>=264实际guard保护；header在size>=296后才写trace和读A/写B；table以实际地址+8比较size。输入size=263/295/583/65536分别触发初始写、header、循环后target边界和正常路径。fresh cell在独立caller-owned config区域，避免逻辑Guest size不足时先偷读Guest高地址；完整arena包含它。seed只写已满足边界的A/无关槽和caller-side fresh cell，未添加伪STORE/入口假设。

x四边值、loop上界0..3、target初值43/44/777/99999、四sizes，每mode256，共2048输入。64个实际表槽独立初始化。normal实际clone两个slot，unknown初始边留原head；unrelated最后写24/25到另一槽，只能给错误提示99/127，不能替代真实state LOAD。OR模式使用528 | (state<<3)，故不能把OR算成ADD；normalhint8/38对应实际OR地址592/816。CALL在路径中，原执行/副作用保留且不复制。

每构建before/normal全部8mode的O0/O2，另plain第一slot partial的256输入×2优化级，共8704正常全状态观察；normal与resumed三次max_rewrites1最终IR应逐字节相同，实际计数[1,1,0]。旧guarded-state-corridors在同源8mode全部byte-noop。bad/zero/1block拒绝不变。复制前后完整state包括return、callcount、73728字节arena（regs、64KiBGuest、7Exit、config和其他字节），不归一化或排除。expected业务/失败分布由独立模型与真实guard确认，不把所有观察都称完整根返回。

8故障：信任hint、错case、LOAD后STORE错误重读、丢table STORE、错duplicate PHI、fresh替代历史值、错误替换state LOAD、丢scratch STORE；每构建4096fault observations，指定应错以独立模型实际差异计数。正常每次11种字段损坏调用相同全状态比较函数，预计95744 comparator controls；50条gzip流/12800帧完整保存审计。此处8704+4096=12800，stages为16普通before/after+1plain partial+8fault，共25/优化级。

四真实ELF及loop observer另组验证；本组不把STORE提示当alias/内存事实，不证明任意未知函数无副作用，不生成去混淆二进制。
