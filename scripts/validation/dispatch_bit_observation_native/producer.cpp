#include <dragon/lifters/llvm/DispatchCorridor.hpp>
#include <llvm/ADT/APInt.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
using namespace dragon::llvmlifter::detail;
std::string decimal(const llvm::APInt &v){llvm::SmallString<48> s;v.toString(s,10,false);return std::string(s);}
std::string print(llvm::Module &m){std::string s;llvm::raw_string_ostream o(s);m.print(o,nullptr);return s;}
int main(int argc,char **argv){
 if(argc!=2||std::filesystem::exists(argv[1]))return 2;
 std::filesystem::create_directories(argv[1]);std::filesystem::path out=argv[1];
 std::ofstream report(out/"rows.tsv");
 for(unsigned w:{8u,16u,32u,64u,128u})for(unsigned mode=0;mode!=4;++mode){
  const std::string t="i"+std::to_string(w),stem="w"+std::to_string(w)+"-m"+std::to_string(mode);
  llvm::APInt mask=llvm::APInt::getOneBitSet(w,mode==0?w-1:mode==1?0:4);
  const unsigned base=mode==0?2:60;
  std::string second=decimal(mask+base),raw;
  if(w<64)raw=" %raw=trunc i64 %lo to "+t+"\n";
  else if(w==64)raw=" %raw=or i64 %lo,0\n";
  else raw=" %low=zext i64 %lo to i128\n %high=zext i64 %hi to i128\n %shifted=shl i128 %high,64\n %raw=or i128 %low,%shifted\n";
  std::string expression;
  if(mode==0)expression=" %bit=and "+t+" %raw,"+decimal(mask)+"\n";
  else if(mode==1)expression=" %bit=lshr "+t+" %raw,"+std::to_string(w-1)+"\n";
  else {
   if(mode==3)expression=" %subtract=sub "+t+" 3,%raw\n %either=or "+t+" %subtract,%raw\n %negate=xor "+t+" %either,-1\n";
   expression+=" %shift=lshr "+t+(mode==3?" %negate,":" %raw,")+std::to_string(w-5)+"\n %bit=and "+t+" %shift,16\n";
  }
  std::string ir="define i32 @f(ptr %guest,ptr %size,i64 %lo,i64 %hi,i1 %take){\nentry: br i1 %take,label %seed,label %other\nseed:\n"+raw+expression+
   " %choice=add "+(mode==0?std::string(""):std::string("nuw nsw "))+t+" %bit,"+std::to_string(base)+"\n br label %head\nother: br label %head\nhead:\n %state=phi "+t+" [%choice,%seed],[7,%other]\n store "+t+" %state,ptr %guest,align 1\n %limit=load i32,ptr %size,align 1\n %fits=icmp uge i32 %limit,"+std::to_string(w/8)+"\n br i1 %fits,label %dispatch,label %fault\ndispatch:\n switch "+t+" %state,label %error ["+t+" "+std::to_string(base)+",label %zero "+t+" "+second+",label %one]\nzero: ret i32 11\none: ret i32 22\nerror: ret i32 33\nfault:\n";
  if(w<32)ir+=" %faultState=zext "+t+" %state to i32\n";
  else if(w==32)ir+=" %faultState=or i32 %state,0\n";
  else ir+=" %faultState=trunc "+t+" %state to i32\n";
  ir+=" %code=xor i32 %faultState,1073741824\n ret i32 %code\n}\n";
  llvm::LLVMContext c;llvm::SMDiagnostic error;auto m=llvm::parseAssemblyString(ir,error,c);
  if(!m||llvm::verifyModule(*m,&llvm::errs()))return 3;
  auto before=print(*m);std::ofstream(out/(stem+"-before.ll"))<<before;
  auto *f=m->getFunction("f");auto block=[&](const char *n){for(auto &b:*f)if(b.getName()==n)return &b;return (llvm::BasicBlock*)nullptr;};
  auto stats=threadDispatchCorridorBatch(*f,{{block("seed"),0,{block("head"),block("dispatch")}}});
  if(llvm::verifyModule(*m,&llvm::errs()))return 4;
  auto after=print(*m);std::ofstream(out/(stem+"-after.ll"))<<after;
  if(!stats.changed&&before!=after)return 5;
  report<<stem<<'\t'<<stats.changed<<'\t'<<stats.complete<<'\t'<<stats.budgetExhausted<<'\t'<<stats.message<<'\n';
 }
}
