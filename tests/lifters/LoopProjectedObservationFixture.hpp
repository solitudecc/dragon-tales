#pragma once
#include "LoopRangeArithmeticFixture.hpp"
namespace {
struct ProjectionFixture {
 enum Mode { Disjoint,High32Only,Low32Clobber,UnknownEffect,AtomicEffect,
             NonDominating,OrphanArm,OtherObservation,BigEndian,TemporalGuard };
 llvm::LLVMContext context;std::unique_ptr<llvm::Module> module;
 explicit ProjectionFixture(Mode mode,bool witnessForm=false) {
  ArithmeticFixture source(ArithmeticFixture::LowOnesSelf);std::string text=source.ir();
  auto replace=[&](const std::string&from,const std::string&to){auto p=text.find(from);REQUIRE(p!=std::string::npos);text.replace(p,from.size(),to);};
  replace("store i64 22, ptr %indexcell, align 1",mode==TemporalGuard ? "" : "store i64 0, ptr %indexcell, align 1");
  auto begin=text.find("a: ");auto end=text.find("a.end:",begin);REQUIRE(begin!=std::string::npos);REQUIRE(end!=std::string::npos);
  std::string body="a:\n";
  if(mode==OrphanArm) body+="  %projection.bypass = icmp eq ptr %runtime, null\n  br i1 %projection.bypass, label %projection.orphan, label %projection.guard\nprojection.orphan:\n  %projection.again = icmp eq ptr %state, %memory\n  br i1 %projection.again, label %projection.orphan, label %projection.work\nprojection.guard:\n";
  body+="  %projection.guardp = getelementptr i8, ptr %guest, i64 "+std::string(mode==OtherObservation?"2056":"2048")+"\n";
  body+="  %projection.old = load i64, ptr %projection.guardp, align 1\n  %projection.old32 = trunc i64 %projection.old to i32\n  %projection.bound = icmp ult i32 %projection.old32, 8\n";
  if(mode==NonDominating) body+="  br i1 %projection.bound, label %projection.yes, label %projection.no\nprojection.yes:\n  br label %projection.work\nprojection.no:\n  br label %projection.work\n";
  else body+="  br i1 %projection.bound, label %projection.work, label %projection.error\n";
  body+="projection.work:\n  %projection.trace = getelementptr i8, ptr %guest, i64 1536\n  store i32 9, ptr %projection.trace, align 1\n";
  if(mode==High32Only) body+="  %projection.high = getelementptr i8, ptr %guest, i64 2052\n  store i32 305419896, ptr %projection.high, align 1\n";
  if(mode==Low32Clobber) body+="  %projection.low = getelementptr i8, ptr %guest, i64 2049\n  store i8 -1, ptr %projection.low, align 1\n";
  if(mode==UnknownEffect) body+="  call void @unknown_call(ptr %guest)\n";
  if(mode==AtomicEffect) body+="  %projection.atomicp = getelementptr i8, ptr %guest, i64 2052\n  store atomic i32 9, ptr %projection.atomicp monotonic, align 4\n";
  body+="  %projection.fresh = load i64, ptr %indexcell, align 1\n  %projection.low32 = trunc i64 %projection.fresh to i32\n  %projection.wide = zext i32 %projection.low32 to i64\n  %projection.scaled = shl i64 %projection.wide, 2\n  %projection.offset = add i64 %projection.scaled, 4008\n  %projection.workp = getelementptr i8, ptr %guest, i64 %projection.offset\n  store i32 8192, ptr %projection.workp, align 1\n  %projection.pointer = load i64, ptr %cell, align 1\n  %projection.output = getelementptr i8, ptr %guest, i64 %projection.pointer\n  store i32 123, ptr %projection.output, align 1\n  %projection.inc = add i32 %projection.low32, 1\n  %projection.next = zext i32 %projection.inc to i64\n  store i64 %projection.next, ptr %indexcell, align 1\n  br label %a.end\nprojection.error:\n  store i64 5, ptr %exit, align 1\n  ret void\n\n";

  if(witnessForm) {
   // Actual full-width Guest traces make both observations business values,
   // as in the real lifted register spills. These are not private keepalives.
   const std::string oldRead="  %projection.old = load i64, ptr %projection.guardp, align 1\n";
   auto keep=body.find(oldRead);REQUIRE(keep!=std::string::npos);
   body.replace(keep,oldRead.size(),oldRead+"  %projection.fulloldp = getelementptr i8, ptr %guest, i64 1560\n  store i64 %projection.old, ptr %projection.fulloldp, align 1\n");
   const std::string freshRead="  %projection.fresh = load i64, ptr %indexcell, align 1\n";
   keep=body.find(freshRead);REQUIRE(keep!=std::string::npos);
   body.replace(keep,freshRead.size(),freshRead+"  %projection.fullfreshp = getelementptr i8, ptr %guest, i64 1552\n  store i64 %projection.fresh, ptr %projection.fullfreshp, align 1\n");
   const std::string original="  %projection.bound = icmp ult i32 %projection.old32, 8\n";
   auto p=body.find(original);REQUIRE(p!=std::string::npos);
   body.replace(p,original.size(),"  %projection.reverse = sub i32 7, %projection.old32\n  %projection.combine = or i32 %projection.reverse, %projection.old32\n  %projection.bound = icmp sgt i32 %projection.combine, -1\n");
   const std::string address="  %projection.wide = zext i32 %projection.low32 to i64\n  %projection.scaled = shl i64 %projection.wide, 2\n  %projection.offset = add i64 %projection.scaled, 4008\n";
   p=body.find(address);REQUIRE(p!=std::string::npos);
   body.replace(p,address.size(),"  %projection.scaled32 = mul i32 %projection.low32, 3\n  %projection.wide = zext i32 %projection.scaled32 to i64\n  %projection.offset = add i64 %projection.wide, 4008\n");
  }
  if(mode==TemporalGuard) body=R"IR(a:
  br label %projection.temporal.head
projection.temporal.head:
  %projection.first = phi i1 [ true, %a ], [ false, %projection.retry ]
  %projection.fresh = load i64, ptr %indexcell, align 1
  %projection.fullfreshp = getelementptr i8, ptr %guest, i64 1552
  store i64 %projection.fresh, ptr %projection.fullfreshp, align 1
  %projection.selected = select i1 %projection.first, i64 29, i64 0
  store i64 %projection.selected, ptr %indexcell, align 1
  br label %projection.current.observe
projection.current.observe:
  %projection.current = load i64, ptr %indexcell, align 1
  %projection.fullcurrentp = getelementptr i8, ptr %guest, i64 1560
  store i64 %projection.current, ptr %projection.fullcurrentp, align 1
  %projection.current32 = trunc i64 %projection.current to i32
  %projection.reverse = sub i32 7, %projection.current32
  %projection.combine = or i32 %projection.reverse, %projection.current32
  %projection.bound = icmp sgt i32 %projection.combine, -1
  br i1 %projection.bound, label %projection.work, label %projection.retry
projection.retry:
  br label %projection.temporal.head
projection.work:
  %projection.low32 = trunc i64 %projection.fresh to i32
  %projection.scaled32 = mul i32 %projection.low32, 3
  %projection.wide = zext i32 %projection.scaled32 to i64
  %projection.offset = add i64 %projection.wide, 4008
  %projection.workp = getelementptr i8, ptr %guest, i64 %projection.offset
  store i32 8192, ptr %projection.workp, align 1
  %projection.pointer = load i64, ptr %cell, align 1
  %projection.output = getelementptr i8, ptr %guest, i64 %projection.pointer
  store i32 123, ptr %projection.output, align 1
  br label %a.end
projection.error:
  store i64 5, ptr %exit, align 1
  ret void

)IR";
  text.replace(begin,end-begin,body);
  begin=text.find("b: ");end=text.find("b.end:",begin);REQUIRE(begin!=std::string::npos);REQUIRE(end!=std::string::npos);
  text.replace(begin,end-begin,"b:\n  br label %b.end\n\n");
  if(mode==TemporalGuard) {
   replace("[ 0, %initialize ]","[ 0, %projection.seed.pass ]");
   replace("  br label %head\n\nhead:",
    "  %projection.seed.observation = load i64, ptr %indexcell, align 1\n"
    "  %projection.seed32 = trunc i64 %projection.seed.observation to i32\n"
    "  %projection.seed.reverse = sub i32 7, %projection.seed32\n"
    "  %projection.seed.combine = or i32 %projection.seed.reverse, %projection.seed32\n"
    "  %projection.seed.bound = icmp sgt i32 %projection.seed.combine, -1\n"
    "  br i1 %projection.seed.bound, label %projection.seed.pass, label %projection.error\n"
    "projection.seed.pass:\n  br label %head\n\nhead:");
   auto at=text.find("b.end:");REQUIRE(at!=std::string::npos);auto jump=text.find("  br label %head",at);REQUIRE(jump!=std::string::npos);text.replace(jump,std::string("  br label %head").size(),"  ret void");
   replace("[ 0, %b.end ], ","");
  }
  if(mode==BigEndian) replace("e-p:64:64","E-p:64:64");
  llvm::SMDiagnostic error;module=llvm::parseAssemblyString(text,error,context);if(!module)error.print("projected-observation",llvm::errs());
  REQUIRE(module);REQUIRE_FALSE(llvm::verifyModule(*module,&llvm::errs()));
 }
 ImageTableContextRequest request()const{return {"view",8192,{4352,4608}};}
 std::string ir()const{std::string text;llvm::raw_string_ostream out(text);module->print(out,nullptr);return text;}
};
}
