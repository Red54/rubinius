#include "vm.hpp"
#include "shared_state.hpp"
#include "call_frame.hpp"
#include "config_parser.hpp"
#include "config.h"
#include "object_memory.hpp"
#include "environment.hpp"
#include "instruments/tooling.hpp"
#include "instruments/timing.hpp"
#include "global_cache.hpp"

#include "util/thread.hpp"
#include "configuration.hpp"

#include "console.hpp"
#include "metrics.hpp"
#include "signal.hpp"
#include "world_state.hpp"
#include "builtin/randomizer.hpp"
#include "builtin/array.hpp"
#include "builtin/thread.hpp"
#include "builtin/native_method.hpp"

#include <iostream>
#include <iomanip>

#include "jit/llvm/state.hpp"

namespace rubinius {

  SharedState::SharedState(Environment* env, Configuration& config, ConfigParser& cp)
    : internal_threads_(0)
    , signals_(0)
    , finalizer_thread_(0)
    , console_(0)
    , metrics_(0)
    , world_(new WorldState(&check_global_interrupts_))
    , method_count_(1)
    , class_count_(1)
    , global_serial_(1)
    , thread_ids_(1)
    , initialized_(false)
    , check_global_interrupts_(false)
    , check_gc_(false)
    , root_vm_(0)
    , env_(env)
    , tool_broker_(new tooling::ToolBroker)
    , fork_exec_lock_()
    , capi_ds_lock_()
    , capi_locks_lock_()
    , capi_constant_lock_()
    , global_capi_handle_lock_()
    , capi_handle_cache_lock_()
    , llvm_state_lock_()
    , vm_lock_()
    , wait_lock_()
    , type_info_lock_()
    , code_resource_lock_()
    , use_capi_lock_(false)
    , om(0)
    , global_cache(new GlobalCache)
    , config(config)
    , user_variables(cp)
    , llvm_state(0)
    , username("")
    , pid("")
  {
    internal_threads_ = new InternalThreads();

    for(int i = 0; i < Primitives::cTotalPrimitives; i++) {
      primitive_hits_[i] = 0;
    }

    hash_seed = Randomizer::random_uint32();
    initialize_capi_black_list();
  }

  SharedState::~SharedState() {
    if(!initialized_) return;

    if(llvm_state) {
      delete llvm_state;
    }

    if(console_) {
      delete console_;
      console_ = 0;
    }

    if(metrics_) {
      delete metrics_;
      metrics_ = 0;
    }

    delete tool_broker_;
    delete global_cache;
    delete world_;
    delete om;
    delete internal_threads_;
  }

  uint32_t SharedState::new_thread_id() {
    return atomic::fetch_and_add(&thread_ids_, 1);
  }

  VM* SharedState::new_vm(const char* name) {
    utilities::thread::SpinLock::LockGuard guard(vm_lock_);

    // TODO calculate the thread id by finding holes in the
    // field of ids, so we reuse ids.
    uint32_t id = new_thread_id();

    VM* vm = new VM(id, *this, name);
    threads_.push_back(vm);

    // If there is no root vm, then the first one created becomes it.
    if(!root_vm_) root_vm_ = vm;

    return vm;
  }

  void SharedState::remove_vm(VM* vm) {
    utilities::thread::SpinLock::LockGuard guard(vm_lock_);

    threads_.remove(vm);

    // Don't delete ourself here, it's too problematic.
  }

  Array* SharedState::vm_threads(STATE) {
    utilities::thread::SpinLock::LockGuard guard(vm_lock_);

    Array* threads = Array::create(state, 0);
    for(ThreadList::iterator i = threads_.begin();
        i != threads_.end();
        ++i) {
      if(VM* vm = (*i)->as_vm()) {
        Thread *thread = vm->thread.get();
        if(!thread->nil_p() && CBOOL(thread->alive())) {
          threads->append(state, thread);
        }
      }
    }

    return threads;
  }

  SignalThread* SharedState::start_signals(STATE) {
    signals_ = new SignalThread(state, state->vm());
    signals_->start(state);

    return signals_;
  }

  console::Console* SharedState::start_console(STATE) {
    if(!console_) {
      console_ = new console::Console(state);
      console_->start(state);
    }

    return console_;
  }

  metrics::Metrics* SharedState::start_metrics(STATE) {
    if(!metrics_) {
      metrics_ = new metrics::Metrics(state);
      metrics_->start(state);
      metrics_->init_ruby_metrics(state);
    }

    return metrics_;
  }

  void SharedState::disable_metrics(STATE) {
    if(metrics_) metrics_->disable(state);
  }

  void SharedState::reset_threads(STATE, GCToken gct, CallFrame* call_frame) {
    VM* current = state->vm();

    for(ThreadList::iterator i = threads_.begin();
           i != threads_.end();
           ++i) {
      if(VM* vm = (*i)->as_vm()) {
        if(vm == current) {
          state->vm()->metrics().system.threads_created++;
          continue;
        }

        if(Thread* thread = vm->thread.get()) {
          if(!thread->nil_p()) {
            thread->unlock_after_fork(state, gct);
            thread->stopped();
          }
        }

        vm->reset_parked();
      }
    }
    threads_.clear();
    threads_.push_back(current);
  }

  void SharedState::after_fork_child(STATE, GCToken gct, CallFrame* call_frame) {
    // For now, we disable inline debugging here. This makes inspecting
    // it much less confusing.

    config.jit_inline_debug.set("no");

    state->vm()->after_fork_child(state);

    disable_metrics(state);

    reset_threads(state, gct, call_frame);

    // Reinit the locks for this object
    global_cache->reset();
    fork_exec_lock_.init();
    capi_ds_lock_.init();
    capi_locks_lock_.init();
    capi_constant_lock_.init();
    global_capi_handle_lock_.init();
    capi_handle_cache_lock_.init();
    llvm_state_lock_.init();
    vm_lock_.init();
    wait_lock_.init();
    type_info_lock_.init();
    code_resource_lock_.init();
    internal_threads_->init();

    om->after_fork_child(state);
    signals_->after_fork_child(state);
    console_->after_fork_child(state);

    state->vm()->set_run_state(ManagedThread::eIndependent);
    gc_dependent(state, 0);
  }

  bool SharedState::should_stop() const {
    return world_->should_stop();
  }

  bool SharedState::stop_the_world(THREAD) {
    return world_->wait_till_alone(state);
  }

  void SharedState::stop_threads_externally() {
    world_->stop_threads_externally();
  }

  void SharedState::reinit_world() {
    world_->reinit();
  }

  void SharedState::restart_world(THREAD) {
    world_->wake_all_waiters(state);
  }

  void SharedState::restart_threads_externally() {
    world_->restart_threads_externally();
  }

  bool SharedState::checkpoint(THREAD) {
    return world_->checkpoint(state);
  }

  void SharedState::gc_dependent(STATE, CallFrame* call_frame) {
    state->set_call_frame(call_frame);
    world_->become_dependent(state->vm());
  }

  void SharedState::gc_independent(STATE, CallFrame* call_frame) {
    state->set_call_frame(call_frame);
    world_->become_independent(state->vm());
  }

  void SharedState::gc_dependent(THREAD, utilities::thread::Condition* cond) {
    world_->become_dependent(state, cond);
  }

  void SharedState::gc_independent(THREAD) {
    world_->become_independent(state);
  }

  void SharedState::gc_independent() {
    world_->become_independent();
  }

  const unsigned int* SharedState::object_memory_mark_address() const {
    return om->mark_address();
  }

  void SharedState::enter_capi(STATE, const char* file, int line) {
    NativeMethodEnvironment* env = state->vm()->native_method_environment;
    if(int lock_index = env->current_native_frame()->capi_lock_index()) {
      capi_locks_[lock_index - 1]->lock();
    }
  }

  void SharedState::leave_capi(STATE) {
    NativeMethodEnvironment* env = state->vm()->native_method_environment;
    if(int lock_index = env->current_native_frame()->capi_lock_index()) {
      capi_locks_[lock_index - 1]->unlock();
    }
  }

  int SharedState::capi_lock_index(std::string name) {
    utilities::thread::SpinLock::LockGuard guard(capi_locks_lock_);
    int existing = capi_lock_map_[name];
    if(existing) return existing;

    CApiBlackList::const_iterator blacklisted = capi_black_list_.find(name);

    // Only disable locks if we have capi locks disabled
    // and library is not in the blacklist.
    if(!use_capi_lock_ && blacklisted == capi_black_list_.end()) {
      capi_lock_map_[name] = 0;
      return 0;
    }

    utilities::thread::Mutex* lock = new utilities::thread::Mutex(true);
    capi_locks_.push_back(lock);

    // We use a 1 offset index, so 0 can indicate no lock used
    int lock_index = capi_locks_.size();
    capi_lock_map_[name] = lock_index;
    return lock_index;
  }

  void SharedState::setup_capi_constant_names() {
    capi_constant_name_map_.resize(cCApiMaxConstant + 1);

    capi_constant_name_map_[cCApiArray]      = "Array";
    capi_constant_name_map_[cCApiBignum]     = "Bignum";
    capi_constant_name_map_[cCApiClass]      = "Class";
    capi_constant_name_map_[cCApiComparable] = "Comparable";
    capi_constant_name_map_[cCApiData]       = "Data";
    capi_constant_name_map_[cCApiEnumerable] = "Enumerable";
    capi_constant_name_map_[cCApiFalse]      = "FalseClass";
    capi_constant_name_map_[cCApiFile]       = "File";
    capi_constant_name_map_[cCApiFixnum]     = "Fixnum";
    capi_constant_name_map_[cCApiFloat]      = "Float";
    capi_constant_name_map_[cCApiHash]       = "Hash";
    capi_constant_name_map_[cCApiInteger]    = "Integer";
    capi_constant_name_map_[cCApiIO]         = "IO";
    capi_constant_name_map_[cCApiKernel]     = "Kernel";
    capi_constant_name_map_[cCApiMatch]      = "MatchData";
    capi_constant_name_map_[cCApiModule]     = "Module";
    capi_constant_name_map_[cCApiNil]        = "NilClass";
    capi_constant_name_map_[cCApiNumeric]    = "Numeric";
    capi_constant_name_map_[cCApiObject]     = "Object";
    capi_constant_name_map_[cCApiRange]      = "Range";
    capi_constant_name_map_[cCApiRegexp]     = "Regexp";
    capi_constant_name_map_[cCApiRubinius]   = "Rubinius";
    capi_constant_name_map_[cCApiString]     = "String";
    capi_constant_name_map_[cCApiStruct]     = "Struct";
    capi_constant_name_map_[cCApiSymbol]     = "Symbol";
    capi_constant_name_map_[cCApiThread]     = "Thread";
    capi_constant_name_map_[cCApiTime]       = "Time";
    capi_constant_name_map_[cCApiTrue]       = "TrueClass";
    capi_constant_name_map_[cCApiProc]       = "Proc";
    capi_constant_name_map_[cCApiGC]         = "GC";
    capi_constant_name_map_[cCApiCAPI]       = "Rubinius::CAPI";
    capi_constant_name_map_[cCApiMethod]     = "Method";
    capi_constant_name_map_[cCApiRational]   = "Rational";
    capi_constant_name_map_[cCApiComplex]    = "Complex";
    capi_constant_name_map_[cCApiEnumerator] = "Enumerable::Enumerator";
    capi_constant_name_map_[cCApiMutex]      = "Mutex";
    capi_constant_name_map_[cCApiDir]        = "Dir";

    capi_constant_name_map_[cCApiArgumentError]       = "ArgumentError";
    capi_constant_name_map_[cCApiEOFError]            = "EOFError";
    capi_constant_name_map_[cCApiErrno]               = "Errno";
    capi_constant_name_map_[cCApiException]           = "Exception";
    capi_constant_name_map_[cCApiFatal]               = "FatalError";
    capi_constant_name_map_[cCApiFloatDomainError]    = "FloatDomainError";
    capi_constant_name_map_[cCApiIndexError]          = "IndexError";
    capi_constant_name_map_[cCApiInterrupt]           = "Interrupt";
    capi_constant_name_map_[cCApiIOError]             = "IOError";
    capi_constant_name_map_[cCApiLoadError]           = "LoadError";
    capi_constant_name_map_[cCApiLocalJumpError]      = "LocalJumpError";
    capi_constant_name_map_[cCApiNameError]           = "NameError";
    capi_constant_name_map_[cCApiNoMemoryError]       = "NoMemoryError";
    capi_constant_name_map_[cCApiNoMethodError]       = "NoMethodError";
    capi_constant_name_map_[cCApiNotImplementedError] = "NotImplementedError";
    capi_constant_name_map_[cCApiRangeError]          = "RangeError";
    capi_constant_name_map_[cCApiRegexpError]         = "RegexpError";
    capi_constant_name_map_[cCApiRuntimeError]        = "RuntimeError";
    capi_constant_name_map_[cCApiScriptError]         = "ScriptError";
    capi_constant_name_map_[cCApiSecurityError]       = "SecurityError";
    capi_constant_name_map_[cCApiSignalException]     = "SignalException";
    capi_constant_name_map_[cCApiStandardError]       = "StandardError";
    capi_constant_name_map_[cCApiSyntaxError]         = "SyntaxError";
    capi_constant_name_map_[cCApiSystemCallError]     = "SystemCallError";
    capi_constant_name_map_[cCApiSystemExit]          = "SystemExit";
    capi_constant_name_map_[cCApiSystemStackError]    = "SystemStackError";
    capi_constant_name_map_[cCApiTypeError]           = "TypeError";
    capi_constant_name_map_[cCApiThreadError]         = "ThreadError";
    capi_constant_name_map_[cCApiZeroDivisionError]   = "ZeroDivisionError";

    capi_constant_name_map_[cCApiMathDomainError]     = "Math::DomainError";
    capi_constant_name_map_[cCApiEncoding]            = "Encoding";
    capi_constant_name_map_[cCApiEncCompatError]      = "Encoding::CompatibilityError";
    capi_constant_name_map_[cCApiWaitReadable]        = "IO::WaitReadable";
    capi_constant_name_map_[cCApiWaitWritable]        = "IO::WaitWritable";

  }

#define CAPI_BLACK_LIST(name) capi_black_list_.insert(std::string("Init_" # name))
  void SharedState::initialize_capi_black_list() {
    CAPI_BLACK_LIST(nkf);
    CAPI_BLACK_LIST(nokogiri);
  }
}
