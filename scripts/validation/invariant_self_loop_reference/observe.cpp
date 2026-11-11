#define _GNU_SOURCE
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <asm/ptrace.h>
#include <elf.h>
#include <link.h>
#include <dlfcn.h>
#include <signal.h>
#include <unistd.h>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
static constexpr std::size_t N=65536, Header=16;
static pid_t child=-1;
[[noreturn]] static void fail(const char *s){perror(s);if(child>0){kill(child,SIGKILL);waitpid(child,nullptr,0);}std::exit(2);}
static void traced(enum __ptrace_request op,void *addr=nullptr,void *data=nullptr){if(ptrace(op,child,addr,data)==-1)fail("ptrace");}
static int status(){int s=0;if(waitpid(child,&s,0)!=child)fail("waitpid");return s;}
static void put(std::uint8_t *p,std::uint64_t v){std::memcpy(p,&v,8);}
static void output(const void *p,std::size_t n){if(fwrite(p,1,n,stdout)!=n)fail("fwrite");}
int main(int argc,char **argv){
 if(argc!=5)return 2;
 auto state=std::strtoull(argv[2],nullptr,0),data=std::strtoull(argv[3],nullptr,0),slot=std::strtoull(argv[4],nullptr,0);if(slot>1)return 2;
 void *lib=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);if(!lib){fprintf(stderr,"%s\n",dlerror());return 2;}
 using Fn=std::uint64_t(*)(void *,void *,void *,std::uint64_t,std::uint64_t,bool);
 auto fn=reinterpret_cast<Fn>(dlsym(lib,"oracle"));if(!fn)return 2;
 Dl_info info{};void *extra=nullptr;if(!dladdr1(reinterpret_cast<void *>(fn),&info,&extra,RTLD_DL_SYMENT)||!extra)return 2;
 const auto size=static_cast<ElfW(Sym)*>(extra)->st_size,start=reinterpret_cast<std::uintptr_t>(fn);if(size<4 || size>65536)return 2;
 auto *shared=static_cast<std::uint8_t *>(mmap(nullptr,Header+N,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0));if(shared==MAP_FAILED)fail("mmap");
 auto *arena=shared+Header;put(shared,0);put(shared+8,0xbed0bed0bed0bed0ULL);
 for(std::size_t n=0;n<N;++n)arena[n]=static_cast<std::uint8_t>((n*37+19)^(n>>8));
 child=fork();if(child<0)fail("fork");
 if(!child){if(ptrace(PTRACE_TRACEME,0,nullptr,nullptr)==-1)_exit(90);raise(SIGSTOP);auto value=fn(arena,arena+1024,arena+60000,state,data,slot);put(shared+8,value);put(shared,1);_exit(0);}
 int s=status();if(!WIFSTOPPED(s)||WSTOPSIG(s)!=SIGSTOP)fail("initial stop");traced(PTRACE_SETOPTIONS,nullptr,reinterpret_cast<void *>(PTRACE_O_EXITKILL));traced(PTRACE_CONT);
 std::uint64_t words[10]={0x4b5350494e303031ULL,0,0,0,0,0,0,size,state,data};std::vector<std::uint8_t> snapshots;
 for(unsigned round=0;round<2;++round){
  usleep(20000);int observed=0;pid_t w=waitpid(child,&observed,WNOHANG);if(w<0)fail("poll");
  if(w==child){if(round || !WIFEXITED(observed)||WEXITSTATUS(observed)!=0)fail("unexpected exit");words[1]=0;words[2]=1;snapshots.assign(shared,shared+Header+N);child=-1;break;}
  if(kill(child,SIGSTOP))fail("stop");s=status();if(!WIFSTOPPED(s)||WSTOPSIG(s)!=SIGSTOP)fail("sample stop");
  user_pt_regs regs{};iovec io{&regs,sizeof(regs)};traced(PTRACE_GETREGSET,reinterpret_cast<void *>(NT_PRSTATUS),&io);if(io.iov_len!=sizeof(regs))fail("register size");
  const std::uintptr_t pc=regs.pc;if(pc<start || pc-start>size-4 || pc%4)fail("PC outside function");std::uint32_t insn=0;std::memcpy(&insn,reinterpret_cast<void *>(pc),4);
  // B with imm26=0 is an unconditional branch to precisely its own address.
  if(insn!=0x14000000u){fprintf(stderr,"non-self branch at +%lu: %08x\n",static_cast<unsigned long>(pc-start),insn);fail("pure self-loop witness");}
  words[1]=1;words[2]=round+1;words[3+round*2]=pc-start;words[4+round*2]=insn;snapshots.insert(snapshots.end(),shared,shared+Header+N);
  if(round==0)traced(PTRACE_CONT);
 }
 if(child>0){traced(PTRACE_KILL);int ended=status();if(!WIFSIGNALED(ended)||WTERMSIG(ended)!=SIGKILL)fail("termination");child=-1;}
 output(words,sizeof(words));output(snapshots.data(),snapshots.size());munmap(shared,Header+N);dlclose(lib);return 0;
}
