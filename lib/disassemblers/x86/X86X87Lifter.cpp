#include "disassemblers/x86/X86IGNILLifter.hpp"
#include <dragon/disassembler/Instruction.hpp>
#include <dragon/lifters/ignil/x86/x86IGNILRegs.hpp>
#include <llvm/MC/MCInst.h>
#include <array>
#include <string>

namespace dragon::x86 {
namespace {
using O=ignil::Opcode;
using V=ignil::Value;
using W=ignil::Width;

// The x87 register file contains raw 80-bit values. All conversions below are
// integer bit operations: no binary64 intermediary can lose extended bits.
struct X87IR {
  ignil::Block &out;
  std::uint32_t &next;
  std::uint64_t address;
  V top,tag,cw,sw,unmasked;
  std::array<std::optional<V>,8> registers;
  std::array<bool,8> changed{};
  bool topChanged=false,tagChanged=false,statusChanged=false;

  V k(uint64_t value,W width) { return ignil::Const{width>=64 ? value : value&((1ull<<width)-1),width}; }
  V emit(O opcode,std::vector<V> operands,W width) {
    const ignil::Temp result{next++,width};
    out.append({opcode,result,std::move(operands),address}); return result;
  }
  V resize(V value,W width,bool sign=false) {
    const auto from=ignil::widthOf(value);
    if (from==width) return value;
    return emit(from>width ? O::TRUNC : sign ? O::SEXT : O::ZEXT,{value},width);
  }
  V bitNot(V bit) { return emit(O::XOR,{bit,k(1,1)},1); }
  V equal(V a,V b) { return emit(O::EQ,{a,b},1); }
  V nonzero(V a) { return bitNot(equal(a,k(0,ignil::widthOf(a)))); }
  V either(V a,V b) { return emit(O::OR,{a,b},1); }
  V both(V a,V b) { return emit(O::AND,{a,b},1); }
  V choose(V condition,V yes,V no,W width) {
    const auto mask=emit(O::NEG,{resize(condition,width)},width);
    return emit(O::OR,{emit(O::AND,{yes,mask},width),emit(O::AND,{no,emit(O::NOT,{mask},width)},width)},width);
  }
  V guard(V value,W width) { return choose(unmasked,emit(O::UNDEF,{},width),value,width); }
  X87IR(ignil::Block &block,std::uint32_t &temp,uint64_t pc):out(block),next(temp),address(pc) {
    top=emit(O::AND,{emit(O::COPY,{FPTOP},8),k(7,8)},8);
    tag=emit(O::COPY,{FPTAG},16); cw=emit(O::COPY,{FPCW},16);sw=emit(O::COPY,{FPSW},16);
    unmasked=k(0,1);
  }
  V index(unsigned logical) { return emit(O::AND,{emit(O::ADD,{top,k(logical,8)},8),k(7,8)},8); }
  V raw(unsigned physical) {
    if (!registers[physical]) registers[physical]=emit(O::COPY,{physicalFPU(physical)},80);
    return *registers[physical];
  }
  V rawAt(V physical) {
    V result=raw(0);
    for (unsigned i=1;i<8;++i) result=choose(equal(physical,k(i,8)),raw(i),result,80);
    return result;
  }
  V tagAt(V physical) {
    const auto shift=emit(O::SHL,{resize(physical,16),k(1,16)},16);
    return resize(emit(O::AND,{emit(O::SHR,{tag,shift},16),k(3,16)},16),8);
  }
  void tagAt(V physical,V value) {
    const auto shift=emit(O::SHL,{resize(physical,16),k(1,16)},16);
    const auto mask=emit(O::SHL,{k(3,16),shift},16);
    tag=emit(O::OR,{emit(O::AND,{tag,emit(O::NOT,{mask},16)},16),emit(O::SHL,{resize(value,16),shift},16)},16);
    tagChanged=true;
  }
  struct Bits { V significand,exponent,sign; };
  Bits bits(V value) {
    const auto high=resize(emit(O::SHR,{value,k(64,8)},80),16);
    return {resize(value,64),emit(O::AND,{high,k(0x7fff,16)},16),resize(emit(O::SHR,{high,k(15,8)},16),1)};
  }
  V pack(V significand,V exponent,V sign) {
    const auto high=emit(O::OR,{exponent,emit(O::SHL,{resize(sign,16),k(15,8)},16)},16);
    return emit(O::OR,{resize(significand,80),emit(O::SHL,{resize(high,80),k(64,8)},80)},80);
  }
  V indefinite() { return pack(k(0xc000000000000000ull,64),k(0x7fff,16),k(1,1)); }
  V classify(V value) {
    const auto b=bits(value);
    const auto zero=both(equal(b.exponent,k(0,16)),equal(b.significand,k(0,64)));
    const auto special=either(either(equal(b.exponent,k(0,16)),equal(b.exponent,k(0x7fff,16))),
      equal(emit(O::SHR,{b.significand,k(63,8)},64),k(0,64)));
    return choose(zero,k(1,8),choose(special,k(2,8),k(0,8),8),8);
  }
  void writeAt(V physical,V value) {
    for (unsigned i=0;i<8;++i) {
      const auto updated=choose(equal(physical,k(i,8)),value,raw(i),80);
      registers[i]=updated; changed[i]=true;
    }
    tagAt(physical,classify(value));
  }
  void exception(unsigned bit,V condition) {
    sw=emit(O::OR,{sw,emit(O::SHL,{resize(condition,16),k(bit,8)},16)},16);
    if (bit<6) {
      const auto enabled=equal(emit(O::AND,{cw,k(1u<<bit,16)},16),k(0,16));
      const auto pending=both(condition,enabled);
      unmasked=either(unmasked,pending);
      sw=emit(O::OR,{sw,choose(pending,k(0x8080,16),k(0,16),16)},16);
    }
    statusChanged=true;
  }
  void conditionCodes(V c1,bool undefinedOthers=true) {
    sw=emit(O::AND,{sw,k(undefinedOthers ? ~0x4700u : ~0x0200u,16)},16);
    if (undefinedOthers) sw=emit(O::OR,{sw,emit(O::AND,{emit(O::UNDEF,{},16),k(0x4500,16)},16)},16);
    sw=emit(O::OR,{sw,emit(O::SHL,{resize(c1,16),k(9,8)},16)},16);
    statusChanged=true;
  }
  struct Read { V value,empty; };
  Read read(unsigned logical) {
    const auto physical=index(logical);
    const auto empty=equal(tagAt(physical),k(3,8));
    exception(0,empty); exception(6,empty);
    return {choose(empty,indefinite(),rawAt(physical),80),empty};
  }
  void push(V value) {
    top=emit(O::AND,{emit(O::SUB,{top,k(1,8)},8),k(7,8)},8); topChanged=true;
    const auto occupied=bitNot(equal(tagAt(top),k(3,8)));
    exception(0,occupied); exception(6,occupied);
    conditionCodes(occupied);
    writeAt(top,choose(occupied,indefinite(),value,80));
  }
  void pop() {
    tagAt(top,k(3,8)); top=emit(O::AND,{emit(O::ADD,{top,k(1,8)},8),k(7,8)},8);topChanged=true;
  }
  V canonicalStatus() {
    return emit(O::OR,{emit(O::AND,{sw,k(~0x3800u,16)},16),emit(O::SHL,{resize(top,16),k(11,8)},16)},16);
  }
  void finish() {
    for (unsigned i=0;i<8;++i) if (changed[i]) out.append({O::COPY,physicalFPU(i),{guard(*registers[i],80)},address});
    if (topChanged) { top=guard(top,8); out.append({O::COPY,FPTOP,{top},address}); }
    if (tagChanged) out.append({O::COPY,FPTAG,{guard(tag,16)},address});
    if (statusChanged || topChanged) out.append({O::COPY,FPSW,{guard(canonicalStatus(),16)},address});
  }
  V clz(V input) {
    V value=input,count=k(0,64);
    for (unsigned width=32;width;width/=2) {
      const auto zero=equal(emit(O::SHR,{value,k(64-width,8)},64),k(0,64));
      value=choose(zero,emit(O::SHL,{value,k(width,8)},64),value,64);
      count=emit(O::ADD,{count,choose(zero,k(width,64),k(0,64),64)},64);
    }
    return emit(O::ADD,{count,resize(equal(value,k(0,64)),64)},64);
  }
  V safeLeft(V value,V amount) {
    const auto large=emit(O::UGE,{amount,k(64,ignil::widthOf(amount))},1);
    const auto safe=choose(large,k(0,ignil::widthOf(amount)),amount,ignil::widthOf(amount));
    return choose(large,k(0,64),emit(O::SHL,{value,safe},64),64);
  }
  V fromInteger(V input) {
    const auto width=ignil::widthOf(input);
    const auto sign=emit(O::SLT,{input,k(0,width)},1);
    const auto magnitude=resize(choose(sign,emit(O::NEG,{input},width),input,width),64);
    const auto leading=clz(magnitude);
    const auto exponent=resize(emit(O::SUB,{k(16383+63,32),resize(leading,32)},32),16);
    return pack(safeLeft(magnitude,leading),choose(equal(magnitude,k(0,64)),k(0,16),exponent,16),sign);
  }
  V fromIEEE(V input,W width,std::optional<V> suppress=std::nullopt) {
    const unsigned fraction=width==32 ? 23 : 52, bias=width==32 ? 127 : 1023;
    const auto sign=resize(emit(O::SHR,{input,k(width-1,8)},width),1);
    const auto exponent=resize(emit(O::AND,{emit(O::SHR,{input,k(fraction,8)},width),k(width==32 ? 255 : 2047,width)},width),16);
    const auto fractionValue=resize(emit(O::AND,{input,k((1ull<<fraction)-1,width)},width),64);
    const auto expZero=equal(exponent,k(0,16));
    const auto special=equal(exponent,k(width==32 ? 255 : 2047,16));
    const auto nan=both(special,nonzero(fractionValue));
    const auto signaling=both(nan,equal(emit(O::AND,{fractionValue,k(1ull<<(fraction-1),64)},64),k(0,64)));
    const auto allowed=suppress ? bitNot(*suppress) : k(1,1);
    exception(0,both(allowed,signaling));exception(1,both(allowed,both(expZero,nonzero(fractionValue))));
    const auto sig=emit(O::SHL,{emit(O::OR,{fractionValue,choose(expZero,k(0,64),k(1ull<<fraction,64),64)},64),k(63-fraction,8)},64);
    const auto leading=clz(sig);
    const auto normal=emit(O::ADD,{emit(O::SUB,{resize(choose(expZero,k(1,16),exponent,16),32),resize(leading,32)},32),k(16383-bias,32)},32);
    const auto resultExponent=choose(special,k(0x7fff,16),choose(equal(sig,k(0,64)),k(0,16),resize(normal,16),16),16);
    const auto normalized=safeLeft(sig,leading);
    const auto quieted=emit(O::OR,{normalized,choose(nan,k(1ull<<62,64),k(0,64),64)},64);
    return pack(quieted,resultExponent,sign);
  }
  struct Classes { Bits b; V zero,nan,unsupported,signaling,denormal,infinity; };
  Classes classes(V value) {
    const auto b=bits(value);
    const auto expZero=equal(b.exponent,k(0,16));
    const auto special=equal(b.exponent,k(0x7fff,16));
    const auto integerBit=resize(emit(O::SHR,{b.significand,k(63,8)},64),1);
    const auto fraction=emit(O::AND,{b.significand,k(0x7fffffffffffffffull,64)},64);
    const auto nan=both(both(special,integerBit),nonzero(fraction));
    return {b,both(expZero,equal(b.significand,k(0,64))),nan,
      both(bitNot(expZero),bitNot(integerBit)),
      both(nan,equal(emit(O::AND,{fraction,k(1ull<<62,64)},64),k(0,64))),
      both(expZero,nonzero(b.significand)),
      both(special,equal(b.significand,k(0x8000000000000000ull,64)))};
  }
  V roundingMode() { return resize(emit(O::AND,{emit(O::SHR,{cw,k(10,8)},16),k(3,16)},16),8); }
  struct Rounded { V value,inexact,increment; };
  Rounded roundRight(V significand,V amount,V mode,V sign) {
    const auto aw=ignil::widthOf(amount);
    const auto large=emit(O::UGE,{amount,k(64,aw)},1);
    const auto safe=choose(large,k(0,aw),amount,aw);
    const auto kept=choose(large,k(0,64),emit(O::SHR,{significand,safe},64),64);
    const auto lowMask=choose(large,k(~0ull,64),emit(O::SUB,{emit(O::SHL,{k(1,64),safe},64),k(1,64)},64),64);
    const auto discarded=emit(O::AND,{significand,lowMask},64);
    const auto inexact=nonzero(discarded);
    const auto halfExists=both(nonzero(amount),emit(O::ULE,{amount,k(64,aw)},1));
    const auto halfShift=choose(halfExists,emit(O::SUB,{amount,k(1,aw)},aw),k(0,aw),aw);
    const auto half=emit(O::SHL,{k(1,64),halfShift},64);
    const auto greater=both(halfExists,emit(O::UGT,{discarded,half},1));
    const auto tie=both(halfExists,equal(discarded,half));
    const auto nearest=either(greater,both(tie,resize(kept,1)));
    const auto directed=either(both(equal(mode,k(1,8)),sign),both(equal(mode,k(2,8)),bitNot(sign)));
    const auto increment=both(inexact,either(both(equal(mode,k(0,8)),nearest),directed));
    return {emit(O::ADD,{kept,resize(increment,64)},64),inexact,increment};
  }
  struct Conversion { V value,inexact,up; };
  Conversion toIEEE(V value,W width) {
    const auto c=classes(value);
    const unsigned fraction=width==32 ? 23 : 52, bias=width==32 ? 127 : 1023;
    const int minimum=1-int(bias),maximum=int(bias);
    const auto leading=clz(c.b.significand);
    const auto sig=safeLeft(c.b.significand,leading);
    const auto exponent=emit(O::SUB,{emit(O::SUB,{resize(choose(equal(c.b.exponent,k(0,16)),k(1,16),c.b.exponent,16),32),k(16383,32)},32),resize(leading,32)},32);
    const auto tiny=emit(O::SLT,{exponent,k(minimum,32)},1);
    const auto amount=choose(tiny,emit(O::SUB,{k(63-fraction+minimum,32),exponent},32),k(63-fraction,32),32);
    const auto mode=roundingMode();
    const auto rounded=roundRight(sig,amount,mode,c.b.sign);
    const auto carry=emit(O::UGE,{rounded.value,k(1ull<<(fraction+1),64)},1);
    const auto after=emit(O::ADD,{exponent,resize(carry,32)},32);
    const auto overflow=emit(O::SGT,{after,k(maximum,32)},1);
    const auto significand=choose(carry,emit(O::SHR,{rounded.value,k(1,8)},64),rounded.value,64);
    const auto normal=emit(O::OR,{emit(O::SHL,{resize(emit(O::ADD,{after,k(bias,32)},32),64),k(fraction,8)},64),
      emit(O::AND,{significand,k((1ull<<fraction)-1,64)},64)},64);
    const auto toInfinity=either(equal(mode,k(0,8)),either(both(equal(mode,k(1,8)),c.b.sign),both(equal(mode,k(2,8)),bitNot(c.b.sign))));
    const uint64_t infinity=uint64_t(width==32 ? 255 : 2047)<<fraction;
    const auto overflowResult=choose(toInfinity,k(infinity,64),k(infinity-1,64),64);
    const auto finiteResult=choose(overflow,overflowResult,choose(tiny,rounded.value,normal,64),64);
    const auto finite=bitNot(either(either(c.nan,c.infinity),c.unsupported));
    const auto inexact=both(finite,either(rounded.inexact,overflow));
    // x87 detects tininess before rounding, even when rounding reaches the
    // minimum normal value (checked against native x86, unlike Unicorn SW).
    const auto underflow=both(both(finite,tiny),rounded.inexact);
    const auto payload=emit(O::OR,{emit(O::AND,{emit(O::SHR,{c.b.significand,k(63-fraction,8)},64),k((1ull<<fraction)-1,64)},64),k(1ull<<(fraction-1),64)},64);
    const auto signedResult=emit(O::OR,{choose(c.nan,emit(O::OR,{k(infinity,64),payload},64),choose(c.infinity,k(infinity,64),finiteResult,64),64),
      emit(O::SHL,{resize(c.b.sign,64),k(width-1,8)},64)},64);
    const auto invalid=either(c.unsupported,c.signaling);
    exception(0,invalid); exception(3,both(finite,overflow)); exception(4,underflow); exception(5,inexact);
    const auto result=choose(c.unsupported,k(infinity|(1ull<<(fraction-1))|(1ull<<(width-1)),64),signedResult,64);
    return {resize(result,width),inexact,both(finite,choose(overflow,toInfinity,rounded.increment,1))};
  }
  Conversion toInteger(V value,W width,bool truncate) {
    const auto c=classes(value);
    const auto exponent=emit(O::SUB,{resize(choose(equal(c.b.exponent,k(0,16)),k(1,16),c.b.exponent,16),32),k(16383,32)},32);
    const auto right=emit(O::SLT,{exponent,k(63,32)},1);
    const auto amount=choose(right,emit(O::SUB,{k(63,32),exponent},32),k(0,32),32);
    const auto rounded=roundRight(c.b.significand,amount,truncate ? k(3,8) : roundingMode(),c.b.sign);
    const auto maximum=choose(c.b.sign,k(1ull<<(width-1),64),k((1ull<<(width-1))-1,64),64);
    const auto overflow=either(emit(O::SGT,{exponent,k(63,32)},1),emit(O::UGT,{rounded.value,maximum},1));
    const auto invalid=either(overflow,either(either(c.nan,c.infinity),c.unsupported));
    const auto inexact=both(bitNot(invalid),rounded.inexact);
    exception(0,invalid); exception(5,inexact);
    const auto signedResult=resize(choose(c.b.sign,emit(O::NEG,{rounded.value},64),rounded.value,64),width);
    return {choose(invalid,k(1ull<<(width-1),width),signedResult,width),inexact,
            truncate ? k(0,1) : both(inexact,rounded.increment)};
  }
};
} // namespace

bool X86IGNILLifter::liftX87(const Instruction &instruction,const AddressContext &context,ignil::Block &out,std::uint32_t &next) const {
  const auto *mc=instruction.mcInst();
  if (!mc) return false;
  const std::string name=instrInfo_->getName(mc->getOpcode()).str();
  bool x87=false,environment=false,vectorRestore=false,unknownSegment=false;
  W unknownStore=0;
  const auto bytes=instruction.bytes();
  bool narrowEnvironment=false;
  for (std::size_t i=0;i<bytes.size();++i) {
    const auto byte=bytes[i];
    if (byte==0x66) narrowEnvironment=true;
    if (byte==0x64 || byte==0x65 || (architecture_==Architecture::X86_32 && (byte==0x26 || byte==0x2e || byte==0x36 || byte==0x3e))) unknownSegment=true;
    if (byte==0x66 || byte==0x67 || byte==0x26 || byte==0x2e || byte==0x36 || byte==0x3e || byte==0x64 || byte==0x65 ||
        byte==0xf0 || byte==0xf2 || byte==0xf3 || (architecture_==Architecture::X86_64 && byte>=0x40 && byte<=0x4f)) continue;
    x87=byte>=0xd8 && byte<=0xdf;
    if ((byte==0xd9 || byte==0xdd) && i+1<bytes.size() && bytes[i+1]<0xc0) {
      const auto group=(bytes[i+1]>>3)&7;
      environment=group==4 || group==6;
      if (group==6) unknownStore=8*(byte==0xd9 ? (narrowEnvironment ? 14 : 28) : (narrowEnvironment ? 94 : 108));
    }
    if (byte==0xdf && i+1<bytes.size() && bytes[i+1]<0xc0 && ((bytes[i+1]>>3)&7)==6) unknownStore=80;
    break;
  }
  if (name=="WAIT" || name=="FNOP") return true;
  if (name.starts_with("FXRSTOR") || name.starts_with("XRSTOR")) { x87=true;environment=true;vectorRestore=true; }
  if (name=="FEMMS") x87=true;
  if (name=="MMX_EMMS") { out.append({O::COPY,FPTAG,{ignil::Const{0xffff,16}},instruction.address()});return true; }
  // MMX aliases physical x87 storage; unsupported MMX must invalidate it.
  for (const auto &operand : *mc)
    if (operand.isReg() && mmxRegisters_.contains(operand.getReg())) x87=true;
  if (!x87) return false;
  const auto address=instruction.address();
  if (name=="FNINIT") {
    out.append({O::COPY,FPTOP,{ignil::Const{0,8}},address});
    out.append({O::COPY,FPTAG,{ignil::Const{0xffff,16}},address});
    out.append({O::COPY,FPCW,{ignil::Const{0x37f,16}},address});
    out.append({O::COPY,FPSW,{ignil::Const{0,16}},address});
    return true;
  }
  X87IR b{out,next,address};
  const W pointerWidth=context.pointerWidth;
  auto memory=[&]() -> V {
    const bool unknownBase=(!zeroAddressRegs_.contains(mc->getOperand(0).getReg()) && !toReg(mc->getOperand(0).getReg())) ||
                           (!zeroAddressRegs_.contains(mc->getOperand(2).getReg()) && !toReg(mc->getOperand(2).getReg()));
    if (unknownSegment || unknownBase) { out.noteUnlifted(address);return b.emit(O::UNDEF,{},pointerWidth); }
    return emitAddress(*mc,0,context,address,out,next);
  };
  auto load=[&](W width) { return b.emit(O::LOAD,{memory()},width); };
  auto store=[&](V value) { out.append({O::STORE,std::nullopt,{memory(),value},address}); };
  auto stackOperand=[&]() -> std::optional<unsigned> {
    if (!mc->getNumOperands() || !mc->getOperand(0).isReg()) return std::nullopt;
    const auto it=x87StackIndex_.find(mc->getOperand(0).getReg());
    return it==x87StackIndex_.end() ? std::nullopt : std::optional<unsigned>{it->second};
  };
  auto widthInName=[&]() -> W {
    if (name.find("16")!=std::string::npos) return 16;
    if (name.find("32")!=std::string::npos) return 32;
    if (name.find("64")!=std::string::npos) return 64;
    return 80;
  };
  if (name=="FLDCW16m") {
    // Reserved CW bits are normalized by hardware; Unicorn stores them raw.
    const auto value=b.emit(O::OR,{b.emit(O::AND,{load(16),b.k(0x1f3f,16)},16),b.k(0x40,16)},16);
    out.append({O::COPY,FPCW,{value},address}); return true;
  }
  if (name=="FNSTCW16m") { store(FPCW); return true; }
  if (name=="FNSTSW16r") { out.append({O::COPY,regAX_,{b.canonicalStatus()},address}); return true; }
  if (name=="FNSTSWm") { store(b.canonicalStatus()); return true; }
  if (name=="FNCLEX") {
    b.sw=b.emit(O::AND,{b.sw,b.k(~0x80ffu,16)},16);b.statusChanged=true;b.finish();return true;
  }
  if (name=="LD_F0" || name=="LD_F1") {
    b.push(b.pack(b.k(name=="LD_F1" ? 0x8000000000000000ull : 0,64),b.k(name=="LD_F1" ? 0x3fff : 0,16),b.k(0,1)));
    b.finish();return true;
  }
  if (name=="LD_F32m" || name=="LD_F64m" || name=="LD_F80m") {
    const auto width=widthInName(); const auto value=load(width);
    b.push(width==80 ? value : b.fromIEEE(value,width));b.finish();return true;
  }
  if (name=="ILD_F16m" || name=="ILD_F32m" || name=="ILD_F64m") {
    b.push(b.fromInteger(load(widthInName())));b.finish();return true;
  }
  if (name=="LD_Frr") if (const auto source=stackOperand()) {
    const auto value=b.read(*source);b.push(value.value);b.finish();return true;
  }
  if (name=="ST_Frr" || name=="ST_FPrr") if (const auto target=stackOperand()) {
    const auto source=b.read(0);
    b.conditionCodes(b.choose(source.empty,b.k(0,1),b.emit(O::UNDEF,{},1),1));
    b.writeAt(b.index(*target),source.value);
    if (name=="ST_FPrr") b.pop();
    b.finish();return true;
  }
  if (name=="ST_F32m" || name=="ST_F64m" || name=="ST_FP32m" || name=="ST_FP64m" || name=="ST_FP80m") {
    const auto width=widthInName();const auto source=b.read(0);
    V value=source.value,c1=b.emit(O::UNDEF,{},1);
    if (width!=80) {
      const auto conversion=b.toIEEE(value,width);value=conversion.value;
      c1=b.choose(conversion.inexact,conversion.up,c1,1);
    }
    b.conditionCodes(b.choose(source.empty,b.k(0,1),c1,1));
    store(b.guard(value,width));
    if (name.starts_with("ST_FP")) b.pop();
    b.finish();return true;
  }
  if (name.starts_with("IST_F") || name.starts_with("ISTT_FP")) {
    const auto width=widthInName();const auto source=b.read(0);
    const auto conversion=b.toInteger(source.value,width,name.starts_with("ISTT_"));
    b.conditionCodes(b.choose(source.empty,b.k(0,1),conversion.up,1));
    store(b.guard(conversion.value,width));
    if (name.starts_with("IST_FP") || name.starts_with("ISTT_FP")) b.pop();
    b.finish();return true;
  }
  if (name=="FFREE") if (const auto target=stackOperand()) {
    b.tagAt(b.index(*target),b.k(3,8));b.conditionCodes(b.emit(O::UNDEF,{},1));b.finish();return true;
  }
  if (name=="FINCSTP" || name=="FDECSTP") {
    b.top=b.emit(O::AND,{b.emit(name=="FINCSTP" ? O::ADD : O::SUB,{b.top,b.k(1,8)},8),b.k(7,8)},8);
    b.topChanged=true;b.conditionCodes(b.k(0,1));b.finish();return true;
  }
  if (name=="XCH_F") if (const auto target=stackOperand()) {
    const auto left=b.read(0),right=b.read(*target);
    b.writeAt(b.index(0),right.value);b.writeAt(b.index(*target),left.value);
    b.conditionCodes(b.k(0,1));b.finish();return true;
  }
  if (name=="CHS_F" || name=="ABS_F") {
    const auto source=b.read(0);
    const auto split=b.bits(source.value);
    const auto value=b.pack(split.significand,split.exponent,name=="CHS_F" ? b.bitNot(split.sign) : b.k(0,1));
    b.writeAt(b.index(0),b.choose(source.empty,b.indefinite(),value,80));
    b.conditionCodes(b.k(0,1));b.finish();return true;
  }
  if (name=="XAM_F") {
    const auto physical=b.index(0),value=b.rawAt(physical);
    const auto c=b.classes(value);
    const auto empty=b.equal(b.tagAt(physical),b.k(3,8));
    const auto denormal=b.both(b.equal(c.b.exponent,b.k(0,16)),b.nonzero(c.b.significand));
    const auto normal=b.bitNot(b.either(b.either(c.zero,denormal),b.either(b.either(c.nan,c.infinity),c.unsupported)));
    const auto c0=b.either(empty,b.either(c.nan,c.infinity));
    const auto c2=b.both(b.bitNot(empty),b.either(normal,b.either(denormal,c.infinity)));
    const auto c3=b.either(empty,b.either(c.zero,denormal));
    V codes=b.emit(O::SHL,{b.resize(c0,16),b.k(8,8)},16);
    for (const auto &[condition,bit] : std::array<std::pair<V,unsigned>,3>{{{c.b.sign,9},{c2,10},{c3,14}}})
      codes=b.emit(O::OR,{codes,b.emit(O::SHL,{b.resize(condition,16),b.k(bit,8)},16)},16);
    b.sw=b.emit(O::OR,{b.emit(O::AND,{b.sw,b.k(~0x4700u,16)},16),codes},16);
    b.statusChanged=true;b.finish();return true;
  }
  const bool compareFlags=name=="COM_FIr" || name=="COM_FIPr" || name=="UCOM_FIr" || name=="UCOM_FIPr";
  const bool compareStack=name=="COM_FST0r" || name=="COMP_FST0r" || name=="UCOM_Fr" || name=="UCOM_FPr" ||
                          name=="FCOMPP" || name=="UCOM_FPPr";
  const bool compareMemory=name=="FCOM32m" || name=="FCOM64m" || name=="FCOMP32m" || name=="FCOMP64m";
  if (compareFlags || compareStack || compareMemory || name=="TST_F") {
    const auto left=b.read(0);
    V right;
    if (compareMemory) {
      const auto width=widthInName();const auto lc=b.classes(left.value);
      right=b.fromIEEE(load(width),width,b.either(lc.nan,lc.unsupported));
    }
    else if (name=="TST_F") right=b.pack(b.k(0,64),b.k(0,16),b.k(0,1));
    else if (const auto source=stackOperand()) right=b.read(*source).value;
    else if (!mc->getNumOperands()) right=b.read(1).value;
    else { out.noteUnlifted(address);return true; }
    const auto a=b.classes(left.value),c=b.classes(right);
    const auto unordered=b.either(b.either(a.nan,c.nan),b.either(a.unsupported,c.unsupported));
    const bool quiet=name.starts_with("UCOM");
    const auto invalid=quiet ? b.either(b.either(a.signaling,c.signaling),b.either(a.unsupported,c.unsupported)) : unordered;
    b.exception(0,invalid);
    b.exception(1,b.both(b.bitNot(unordered),b.either(a.denormal,c.denormal)));
    // Pseudo-denormals have the same value as exponent-one encodings.
    const auto ae=b.choose(b.equal(a.b.exponent,b.k(0,16)),b.k(1,16),a.b.exponent,16);
    const auto ce=b.choose(b.equal(c.b.exponent,b.k(0,16)),b.k(1,16),c.b.exponent,16);
    const auto exponentEqual=b.equal(ae,ce),signEqual=b.equal(a.b.sign,c.b.sign);
    const auto zeros=b.both(a.zero,c.zero);
    const auto equal=b.both(b.bitNot(unordered),b.either(zeros,b.both(signEqual,b.both(exponentEqual,b.equal(a.b.significand,c.b.significand)))));
    const auto smaller=b.either(b.emit(O::ULT,{ae,ce},1),b.both(exponentEqual,b.emit(O::ULT,{a.b.significand,c.b.significand},1)));
    const auto larger=b.either(b.emit(O::UGT,{ae,ce},1),b.both(exponentEqual,b.emit(O::UGT,{a.b.significand,c.b.significand},1)));
    const auto less=b.both(b.bitNot(b.either(zeros,unordered)),b.choose(signEqual,b.choose(a.b.sign,larger,smaller,1),a.b.sign,1));
    const auto carry=b.either(less,unordered),zero=b.either(equal,unordered);
    if (compareFlags) {
      for (const auto &[reg,condition] : std::array<std::pair<ignil::Reg,V>,3>{{{regCF_,carry},{regZF_,zero},{regPF_,unordered}}})
        out.append({O::COPY,reg,{b.choose(b.unmasked,reg,b.resize(condition,8),8)},address});
      for (const auto flag : {regOF_,regSF_,regAF_}) out.append({O::COPY,flag,{b.k(0,8)},address});
      b.conditionCodes(b.k(0,1),false);
    } else {
      const auto conditions=b.emit(O::OR,{b.emit(O::SHL,{b.resize(carry,16),b.k(8,8)},16),
        b.emit(O::OR,{b.emit(O::SHL,{b.resize(unordered,16),b.k(10,8)},16),b.emit(O::SHL,{b.resize(zero,16),b.k(14,8)},16)},16)},16);
      const auto old=b.emit(O::AND,{b.sw,b.k(0x4500,16)},16);
      b.sw=b.emit(O::OR,{b.emit(O::AND,{b.sw,b.k(~0x4700u,16)},16),b.choose(b.unmasked,old,conditions,16)},16);
      b.statusChanged=true;
    }
    if (name=="FCOMPP" || name=="UCOM_FPPr") { b.pop();b.pop(); }
    else if (name=="COM_FIPr" || name=="UCOM_FIPr" || name=="COMP_FST0r" || name=="UCOM_FPr" || name.starts_with("FCOMP")) b.pop();
    b.finish();return true;
  }
  // Arithmetic that needs extended precision/PC, and unmodelled environment
  // formats, explicitly destroy the physical stack state. No double fallback.
  if (vectorRestore) for (unsigned i=0;i<(architecture_==Architecture::X86_64 ? 16u : 8u);++i) {
    emitUndef(regXmmLo_[i],address,out);emitUndef(regXmmHi_[i],address,out);
  }
  if (unknownStore) store(b.emit(O::UNDEF,{},unknownStore));
  for (unsigned i=0;i<8;++i) emitUndef(physicalFPU(i),address,out);
  for (const auto reg : {FPTOP,FPTAG,FPSW}) emitUndef(reg,address,out);
  if (environment) emitUndef(FPCW,address,out);
  out.noteUnlifted(address);
  return true;
}
} // namespace dragon::x86
