#pragma once
namespace clang { class Decl; class Stmt; class GlobalDecl; }
namespace llvm { class Function; class CallBase; }
extern "C" void cxxlens_eh_function(void*,const clang::Decl*,llvm::Function*,bool);
extern "C" void cxxlens_eh_variant(void*,const clang::GlobalDecl*);
extern "C" void cxxlens_eh_expression(void*,const clang::Decl*,const clang::Stmt*,unsigned,bool);
extern "C" void cxxlens_eh_call_admit(void*,const clang::Decl*,const clang::Decl*);
extern "C" void cxxlens_eh_call_emit(void*,const clang::Decl*,const clang::Decl*,llvm::CallBase*,bool);
extern "C" void cxxlens_eh_builtin(void*,const clang::Decl*,const clang::Decl*,const clang::Stmt*,unsigned,bool);
extern "C" void cxxlens_eh_runtime_emit(void*,const clang::Decl*,llvm::CallBase*,unsigned);
struct cxxlens_eh_builtin_scope {
  void* context; const clang::Decl* owner; const clang::Decl* target;
  const clang::Stmt* expression; unsigned builtin;
  cxxlens_eh_builtin_scope(void* c,const clang::Decl* o,const clang::Decl* t,const clang::Stmt* e,unsigned b)
    :context(c),owner(o),target(t),expression(e),builtin(b) {
    cxxlens_eh_builtin(context,owner,target,expression,builtin,true);
  }
  ~cxxlens_eh_builtin_scope() {
    cxxlens_eh_builtin(context,owner,target,expression,builtin,false);
  }
};
struct cxxlens_eh_expression_scope {
  void* context; const clang::Decl* owner; const clang::Stmt* expression; unsigned kind;
  cxxlens_eh_expression_scope(void* c,const clang::Decl* o,const clang::Stmt* e,unsigned k)
    :context(c),owner(o),expression(e),kind(k) { cxxlens_eh_expression(c,o,e,k,true); }
  ~cxxlens_eh_expression_scope() { cxxlens_eh_expression(context,owner,expression,kind,false); }
};
struct cxxlens_eh_finish_scope {
 void* context; const clang::Decl* owner; llvm::Function* function;
 ~cxxlens_eh_finish_scope() { cxxlens_eh_function(context,owner,function,false); }
};

extern "C" unsigned long long cxxlens_eh_cleanup_register(void*, const clang::Decl*, const clang::Decl*, unsigned);
extern "C" void cxxlens_eh_cleanup_emit(void*, const clang::Decl*, unsigned long long, bool, bool);
struct cxxlens_eh_cleanup_scope {
  void* context; const clang::Decl* owner; unsigned long long registration; bool exceptional;
  cxxlens_eh_cleanup_scope(void* c, const clang::Decl* o, unsigned long long r, bool e)
    : context(c), owner(o), registration(r), exceptional(e) {
    cxxlens_eh_cleanup_emit(context, owner, registration, exceptional, true);
  }
  ~cxxlens_eh_cleanup_scope() {
    cxxlens_eh_cleanup_emit(context, owner, registration, exceptional, false);
  }
};
extern "C" void cxxlens_eh_destructor_target(void*, const clang::Decl*, const clang::Decl*, unsigned, bool);
struct cxxlens_eh_destructor_target_scope {
  void* context; const clang::Decl* owner; const clang::Decl* target; unsigned kind;
  cxxlens_eh_destructor_target_scope(void* c, const clang::Decl* o, const clang::Decl* t, unsigned k)
    : context(c), owner(o), target(t), kind(k) {
    cxxlens_eh_destructor_target(context, owner, target, kind, true);
  }
  ~cxxlens_eh_destructor_target_scope() {
    cxxlens_eh_destructor_target(context, owner, target, kind, false);
  }
};
