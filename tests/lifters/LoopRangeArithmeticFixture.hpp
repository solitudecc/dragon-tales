#pragma once
#include <catch2/catch_test_macros.hpp>
#include <dragon/lifters/llvm/ImageTableContext.hpp>
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include <memory>
#include <string>
using namespace dragon::llvmlifter::detail;
namespace {
// Guard/SP/whole-call skeleton copied from the public LoopFixture, not linked
// to a source-inclusion Image implementation. Existing tests remain unchanged.
// Source SHA256: c4f54ddd08c7e5f10520aedf1d4b83331b6c6c4c7afe8e7a51a1d7e20eecb3a4
struct ArithmeticFixture {
 enum Mode { Sparse32,Sparse64,SafeFlags32,SafeFlags64,ZeroUnknown,
   LowOnesSelf,WideLowOnes,UnknownX,Overflow,ActualCellOverlap,PartialCellOverlap,
   WrappedAddress,UnknownCall,Poison,Undef,IndependentFreeze,FlaggedUnknown };
 llvm::LLVMContext context; std::unique_ptr<llvm::Module> module;
 explicit ArithmeticFixture(Mode mode) {
  std::string text=R"IR(
target datalayout = "e-p:64:64"
declare void @ordinary(ptr, ptr, ptr, ptr)
declare void @unknown_call(ptr)
define void @view(ptr %state, ptr %memory, ptr %runtime, ptr %exit) {
entry:
  %slot = alloca i64
  %pcslot = alloca i64
  %regdesc = getelementptr i8, ptr %state, i64 24
  %regs = load ptr, ptr %regdesc, align 1
  %guest = load ptr, ptr %memory, align 1
  %spaddr = getelementptr i8, ptr %regs, i64 32
  %initialsp = load i64, ptr %spaddr, align 1
  store i64 %initialsp, ptr %slot
  %outaddr = getelementptr i8, ptr %regs, i64 48
  %snapshot = load i64, ptr %outaddr, align 1
  %pcaddr = getelementptr i8, ptr %regs, i64 128
  %initialpc = load i64, ptr %pcaddr, align 1
  store i64 %initialpc, ptr %pcslot
  %pc = load i64, ptr %pcslot
  switch i64 %pc, label %unsupported [ i64 4096, label %bounds ]
bounds:
  %sizeaddr = getelementptr i8, ptr %memory, i64 8
  %size = load i64, ptr %sizeaddr, align 1
  %large = icmp uge i64 %size, 65536
  %nonnull = icmp ne ptr %guest, null
  %valid = and i1 %large, %nonnull
  br i1 %valid, label %spcheck, label %fallback
spcheck:
  %spok = icmp eq i64 %initialsp, 49152
  br i1 %spok, label %outcheck, label %fallback
outcheck:
  %outok = icmp eq i64 %snapshot, 32768
  br i1 %outok, label %table0, label %fallback
table0:
  %t0p = getelementptr i8, ptr %guest, i64 8192
  %t0 = load i64, ptr %t0p, align 1
  %t0ok = icmp eq i64 %t0, 4352
  br i1 %t0ok, label %table1, label %fallback
table1:
  %t1p = getelementptr i8, ptr %guest, i64 8200
  %t1 = load i64, ptr %t1p, align 1
  %t1ok = icmp eq i64 %t1, 4608
  br i1 %t1ok, label %accepted, label %fallback
accepted:
  br label %initialize
initialize:
  %sp0 = load i64, ptr %slot
  %frame = sub i64 %sp0, 64
  store i64 %frame, ptr %slot
  %cell = getelementptr i8, ptr %guest, i64 4096
  store i64 %snapshot, ptr %cell, align 1
  
  %indexcell = getelementptr i8, ptr %guest, i64 2048
  store i64 22, ptr %indexcell, align 1
  br label %head
head:
  %statevalue = phi i64 [ 0, %initialize ], [ 1, %a.end ], [ 0, %b.end ], [ %state.step, %head ]
  %state.step = add i64 %statevalue, 1
  %inrange = icmp ult i64 %statevalue, 2
  br i1 %inrange, label %read, label %head
read:
  %scale = shl i64 %statevalue, 3
  %tableoff = add i64 %scale, 8192
  %targetp = getelementptr i8, ptr %guest, i64 %tableoff
  %target = load i64, ptr %targetp, align 1
  switch i64 %target, label %unknown [ i64 4352, label %a i64 4608, label %b ]
a:
  %tracep = getelementptr i8, ptr %guest, i64 1536
  store i64 %statevalue, ptr %tracep, align 1
  
  %datap = getelementptr i8, ptr %guest, i64 128
  %data = load i64, ptr %datap, align 1
  %bounded = and i64 %data, 7
  store i64 %bounded, ptr %indexcell, align 1
  
  %index.a = load i64, ptr %indexcell, align 1
  %scaled.a = shl i64 %index.a, 2
  %offset.a = add i64 %scaled.a, 4008
  %work.a = getelementptr i8, ptr %guest, i64 %offset.a
  store i32 8192, ptr %work.a, align 1
  %pointer = load i64, ptr %cell, align 1
  %output = getelementptr i8, ptr %guest, i64 %pointer
  store i32 123, ptr %output, align 1
  
  br label %a.end
a.end:
  br label %head
b:
  %index.b = load i64, ptr %indexcell, align 1
  %scaled.b = shl i64 %index.b, 2
  %offset.b = add i64 %scaled.b, 4008
  %work.b = getelementptr i8, ptr %guest, i64 %offset.b
  store i32 8192, ptr %work.b, align 1
  %pointer.b = load i64, ptr %cell, align 1
  %output.b = getelementptr i8, ptr %guest, i64 %pointer.b
  store i32 321, ptr %output.b, align 1
  
  br label %b.end
b.end:
  br label %head
unknown:
  store i64 %target, ptr %exit, align 1
  ret void
unsupported:
  ret void
fallback:
  call void @ordinary(ptr %state, ptr %memory, ptr %runtime, ptr %exit)
  ret void
}
)IR";
  auto replace=[&](const std::string&from,const std::string&to) {
   auto at=text.find(from);REQUIRE(at!=std::string::npos);
   text.replace(at,from.size(),to);
  };
  const bool w32=mode==Sparse32||mode==SafeFlags32;
  const bool flags=mode==SafeFlags32||mode==SafeFlags64||mode==FlaggedUnknown;
  const std::string ty=w32?"i32":"i64";
  for (const std::string suffix:{"a","b"}) {
   std::string code;
   const std::string x="%arith.x."+suffix;
   if (mode==LowOnesSelf) continue;
   if (w32) code="  "+x+" = trunc i64 %index."+suffix+" to i32\n";
   else code="  "+x+" = add i64 %index."+suffix+", 0\n";
   if (mode==UnknownX||mode==Overflow||mode==FlaggedUnknown) {
    code="  %arith.inputp."+suffix+" = getelementptr i8, ptr %guest, i64 256\n";
    code+="  "+x+" = load i64, ptr %arith.inputp."+suffix+", align 1\n";
   }
   if (mode==Poison||mode==Undef)
    code="  "+x+" = add i64 "+(mode==Poison?"poison":"undef")+", 0\n";
   if (mode==IndependentFreeze) {
    // Independent real observations remain semantically independent even if
    // normalization removes redundant freezes. freeze undef may legally fold
    // to zero and was not a valid rejection fixture (v2 evidence retained).
    code="  %freeze.px."+suffix+" = getelementptr i8, ptr %guest, i64 256\n";
    code+="  %freeze.py."+suffix+" = getelementptr i8, ptr %guest, i64 264\n";
    code+="  %freeze.x."+suffix+" = load i64, ptr %freeze.px."+suffix+", align 1\n";
    code+="  %freeze.y."+suffix+" = load i64, ptr %freeze.py."+suffix+", align 1\n";
    code+="  "+x+" = freeze i64 %freeze.x."+suffix+"\n";
    code+="  %arith.other."+suffix+" = freeze i64 %freeze.y."+suffix+"\n";
   }
   const std::string y=mode==IndependentFreeze?"%arith.other."+suffix:x;
   if (mode==Undef) {
    // Keep undef as the actual address input instead of embedding it in MBA,
    // whose zero choice is a legal LLVM refinement. No assertion is relaxed.
    code="  %scaled."+suffix+" = add i64 undef, 0";
   } else if (mode==WideLowOnes) {
    code="  %wide.tagp."+suffix+" = getelementptr i8, ptr %guest, i64 128\n";
    code+="  %wide.tag."+suffix+" = load i64, ptr %wide.tagp."+suffix+", align 1\n";
    code+="  %wide.low."+suffix+" = and i64 %wide.tag."+suffix+", 3\n";
    code+="  %wide.shift."+suffix+" = shl i64 %index."+suffix+", 2\n";
    code+="  %wide.join."+suffix+" = or disjoint i64 %wide.shift."+suffix+", %wide.low."+suffix+"\n";
    code+="  %scaled."+suffix+" = and i64 %wide.join."+suffix+", 4294967295";
   } else if (mode==ZeroUnknown) {
    code="  %arith.inputp."+suffix+" = getelementptr i8, ptr %guest, i64 256\n";
    code+="  "+x+" = load i64, ptr %arith.inputp."+suffix+", align 1\n";
    code+="  %scaled."+suffix+" = mul i64 "+x+", 0";
   } else if (mode==Overflow||mode==FlaggedUnknown) {
    code+="  %scaled."+suffix+" = mul "+(flags?"nuw nsw ":"")+"i64 "+x+", 9223372036854775809";
   } else {
    code+="  %arith.bit."+suffix+" = and "+ty+" "+x+", 4\n";
    code+="  %arith.set."+suffix+" = or "+ty+" "+x+", 4\n";
    code+="  %arith.flip."+suffix+" = xor "+ty+" %arith.bit."+suffix+", 4\n";
    code+="  %arith.clear."+suffix+" = and "+ty+" "+y+", -5\n";
    code+="  %arith.first."+suffix+" = mul "+(flags?"nuw nsw ":"")+ty+" %arith.bit."+suffix+", %arith.set."+suffix+"\n";
    code+="  %arith.second."+suffix+" = mul "+(flags?"nuw nsw ":"")+ty+" %arith.clear."+suffix+", %arith.flip."+suffix+"\n";
    code+="  %arith.total."+suffix+" = add "+ty+" %arith.first."+suffix+", %arith.second."+suffix+"\n";
    code+="  %scaled."+suffix+" = "+(w32?"zext i32 %arith.total."+suffix+" to i64":"add i64 %arith.total."+suffix+", 0");
   }
   replace("  %scaled."+suffix+" = shl i64 %index."+suffix+", 2",code);
   const std::string base=mode==ActualCellOverlap?"4096":mode==PartialCellOverlap?"4093":mode==WrappedAddress?"-2":"4008";
   replace("%offset."+suffix+" = add i64 %scaled."+suffix+", 4008","%offset."+suffix+" = add i64 %scaled."+suffix+", "+base);
  }
  if (mode==UnknownCall) replace("  br label %a.end","  call void @unknown_call(ptr %guest)\n  br label %a.end");
  if (mode==LowOnesSelf) replace("%datap = getelementptr i8, ptr %guest, i64 128","%datap = getelementptr i8, ptr %guest, i64 2048");
  llvm::SMDiagnostic error;module=llvm::parseAssemblyString(text,error,context);
  if(!module)error.print("range-arithmetic",llvm::errs());REQUIRE(module);
  REQUIRE_FALSE(llvm::verifyModule(*module,&llvm::errs()));
 }
 ImageTableContextRequest request()const{return {"view",8192,{4352,4608}};}
 std::string ir()const{std::string s;llvm::raw_string_ostream out(s);module->print(out,nullptr);return s;}
};
}
