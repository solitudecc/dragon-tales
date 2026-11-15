#include <string>
std::string carriedBatchLoop(unsigned count) {
 std::string ir="define i32 @f(ptr %guest,ptr %size,i32 %raw){\nentry:\n";
 for(unsigned j=0;j<20;++j)
  ir+=" %initial"+std::to_string(j)+"=xor i32 %raw,"+std::to_string(0x9e37u*(j+1))+"\n";
 ir+=" br label %head\nhead:\n %state=phi i32 [0,%entry]";
 for(unsigned n=0;n<count;++n)ir+=",["+std::to_string((n+1)%count)+",%seed"+std::to_string(n)+"]";
 ir+="\n %counter=phi i32 [0,%entry]";
 for(unsigned n=0;n<count;++n)ir+=",[%advanced,%seed"+std::to_string(n)+"]";
 for(unsigned j=0;j<20;++j){
  const auto id=std::to_string(j);ir+="\n %carried"+id+"=phi i32 [%initial"+id+",%entry]";
  for(unsigned n=0;n<count;++n)ir+=",[%next"+std::to_string(n)+"_"+id+",%seed"+std::to_string(n)+"]";
 }
 ir+="\n %advanced=add i32 %counter,1\n %stamp=xor i32 %carried0,%carried1\n"
     " store i32 %stamp,ptr %guest,align 1\n %limit=load i32,ptr %size,align 1\n"
     " %fits=icmp ule i32 %advanced,%limit\n %bounded=icmp ule i32 %advanced,256\n"
     " %continue=and i1 %fits,%bounded\n br i1 %continue,label %dispatch,label %fault\n"
     "dispatch: switch i32 %state,label %error [";
 for(unsigned n=0;n<count;++n)ir+="i32 "+std::to_string(n)+",label %seed"+std::to_string(n)+" ";
 ir+="]\n";
 for(unsigned n=0;n<count;++n){
  const auto id=std::to_string(n);ir+="seed"+id+":\n";
  for(unsigned j=0;j<20;++j)
   ir+=" %next"+id+"_"+std::to_string(j)+"=add i32 %carried"+std::to_string(j)+","+std::to_string(n+j+1)+"\n";
  ir+=" br label %head\n";
 }
 ir+="fault:\n %result0=xor i32 %advanced,%carried0\n";
 for(unsigned j=1;j<20;++j)
  ir+=" %result"+std::to_string(j)+"=xor i32 %result"+std::to_string(j-1)+",%carried"+std::to_string(j)+"\n";
 ir+=" ret i32 %result19\nerror: ret i32 %advanced\n}\n";
 return ir;
}
