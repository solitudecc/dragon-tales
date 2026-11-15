#include <string>
#include <stdexcept>
static void fixtureRequire(bool v){if(!v)throw std::runtime_error("fixture generation failed");}
std::string loop(const std::string &largeSeed="0",const std::string &smallSeed="0",
                 const std::string &entry="",const std::string &step="add i32 %low,1",
                 const std::string &extend="zext",const std::string &low="trunc i64 %large to i32") {
  return "define i32 @f(ptr %memory,i32 %arg,i1 %take){\nentry:\n"+entry+R"IR(
 br label %head
head:
 %large=phi i64 [)IR"+largeSeed+R"IR(,%entry],[%wide,%back]
 %small=phi i32 [)IR"+smallSeed+R"IR(,%entry],[%next,%back]
 %low=)IR"+low+"\n %next="+step+"\n %wide="+extend+R"IR( i32 %next to i64
 store i32 %next,ptr %memory,align 1
 %more=icmp ult i32 %next,4
 br i1 %more,label %back,label %done
back: br label %head
done: ret i32 %next
})IR";
}
std::string relationForwardedLoop(unsigned count,bool duplicate=false,const std::string &entry="",
                          const std::string &seed="%initial") {
 std::string setup=" %initial=freeze i32 %arg\n"+entry+" %initialWide=zext i32 "+seed+" to i64\n";
 auto ir=loop("%initialWide",seed,setup);
 const std::string wide=" %wide=zext i32 %next to i64\n";
 auto at=ir.find(wide);fixtureRequire(at!=std::string::npos);ir.erase(at,wide.size());
 at=ir.find("[%next,%back]");fixtureRequire(at!=std::string::npos);
 ir.replace(at,std::string("[%next,%back]").size(),"[%forward"+std::to_string(count-1)+",%back]");
 at=ir.find("label %back,label %done");fixtureRequire(at!=std::string::npos);
 ir.replace(at,std::string("label %back,label %done").size(),"label %chain0,label %done");
 std::string chain;
 for(unsigned n=0;n<count;++n){
  const auto id=std::to_string(n),previous=n?"chain"+std::to_string(n-1):"head";
  const auto value=n?"%forward"+std::to_string(n-1):"%next";
  chain+="chain"+id+":\n %forward"+id+"=phi i32 ["+value+",%"+previous+"]";
  if(duplicate&&n)chain+=",["+value+",%"+previous+"]";
  const auto next=n+1<count?"chain"+std::to_string(n+1):"back";
  chain+=duplicate&&n+1<count?"\n br i1 %take,label %"+next+",label %"+next+"\n":"\n br label %"+next+"\n";
 }
 chain+="back:\n %wide=zext i32 %forward"+std::to_string(count-1)+" to i64\n br label %head";
 at=ir.find("back: br label %head");fixtureRequire(at!=std::string::npos);ir.replace(at,std::string("back: br label %head").size(),chain);
 return ir;
}
std::string forwardedLoop(unsigned count,bool duplicate=false,const std::string &entry="",
                          const std::string &seed="%initial") {
 auto ir=relationForwardedLoop(count,duplicate,entry,seed);
 auto replace=[&](const std::string &from,const std::string &to){
  auto at=ir.find(from);fixtureRequire(at!=std::string::npos);ir.replace(at,from.size(),to);
 };
 // The relation sees the same non-PHI step on both arms. Definedness must
 // still traverse every forwarding PHI feeding that step, as in the real
 // failing definition query. Keep the earlier relation-stress shape below.
 replace(" %next=add i32 %low,1"," %observed=add i32 %low,1");
 replace("store i32 %next,ptr","store i32 %observed,ptr");
 replace("icmp ult i32 %next,4","icmp ult i32 %observed,4");
 replace("done: ret i32 %next","done: ret i32 %observed");
 replace("[%forward"+std::to_string(count-1)+",%back]","[%next,%back]");
 replace("[%next,%head]","[%low,%head]");
 replace("back:\n %wide=zext i32 %forward"+std::to_string(count-1)+" to i64",
         "back:\n %next=add i32 %forward"+std::to_string(count-1)+",1\n %wide=zext i32 %next to i64");
 return ir;
}